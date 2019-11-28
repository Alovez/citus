/*-------------------------------------------------------------------------
 *
 * repartition_logic.c
 *
 * This file contains repartition specific logic.
 * ExecuteDependedTasks takes a list of top level tasks. Its logic is as follows:
 * - It generates all the tasks by descending in the tasks tree. Note that each task
 *  has a dependedTaskList.
 * - It generates FetchTask queryStrings with the MapTask queries. It uses the first replicate to
 *  fetch data when replication factor is > 1. Note that if a task fails in any replica adaptive executor
 *  gives an error, so if we come to a fetchTask we know for sure that its dependedMapTask is executed in all
 *  replicas.
 * - It creates schemas in each worker in a single transaction to store intermediate results.
 * - It iterates all tasks and finds the ones whose dependencies are already executed, and executes them with
 *  adaptive executor logic.
 *
 *
 * Repartition queries do not begin a transaction even if we are in
 * a transaction block. As we dont begin a transaction, they wont see the
 * DDLs that happened earlier in the transaction because we dont have that
 * transaction id with repartition queries. Therefore we error in this case.
 *
 * Copyright (c) Citus Data, Inc.
 */

#include "postgres.h"
#include "access/hash.h"
#include "distributed/hash_helpers.h"

#include "distributed/directed_acylic_graph_execution_logic.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/adaptive_executor.h"
#include "distributed/worker_manager.h"
#include "distributed/multi_server_executor.h"
#include "distributed/repartition_logic.h"
#include "distributed/worker_transaction.h"
#include "distributed/worker_manager.h"
#include "distributed/transaction_management.h"
#include "distributed/multi_task_tracker_executor.h"
#include "distributed/metadata_cache.h"
#include "distributed/transmit.h"


static void FillTaskGroups(List **allTasks, List **mergeTasks);
static List * CreateTemporarySchemas(List *mergeTasks);
static List * CreateJobIds(List *mergeTasks);
static void CreateSchemasOnAllWorkers(char *createSchemasCommand);
static char * GenerateCreateSchemasCommand(List *jobIds);
static char * GenerateCommand(List *jobIds, char *templateCommand);
static bool DoesJobIDExist(List *jobIds, uint64 jobId);
static void SendCommandToAllWorkers(List *commandList);
static char * GenerateDeleteJobsCommand(List *jobIds);
static void RemoveTempJobDirs(List *jobIds);


/*
 * ExecuteDependedTasks executes all tasks except the top level tasks
 * in order from the task tree. At a time, it can execute different tasks from
 * different jobs.
 */
void
ExecuteDependedTasks(List *topLevelTasks)
{
	List *mergeTasks = NIL;


	EnsureNoModificationsHaveBeenDone();

	List *allTasks = TaskAndExecutionList(topLevelTasks);

	FillTaskGroups(&allTasks, &mergeTasks);

	List *jobIds = CreateTemporarySchemas(mergeTasks);

	ExecuteTasksInDependencyOrder(allTasks, topLevelTasks);

	RemoveTempJobDirs(jobIds);
}


/*
 * FillTaskGroups iterates all tasks and creates a group for mergeTasks.
 */
static void
FillTaskGroups(List **allTasks, List **mergeTasks)
{
	ListCell *taskCell = NULL;

	foreach(taskCell, *allTasks)
	{
		Task *task = (Task *) lfirst(taskCell);

		if (task->taskType == MERGE_TASK)
		{
			*mergeTasks = lappend(*mergeTasks, task);
		}
	}
}


/*
 * CreateTemporarySchemas creates the necessary schemas that will be used
 * later in each worker. Single transaction is used to create the schemas.
 */
static List *
CreateTemporarySchemas(List *mergeTasks)
{
	List *jobIds = CreateJobIds(mergeTasks);
	char *createSchemasCommand = GenerateCreateSchemasCommand(jobIds);
	CreateSchemasOnAllWorkers(createSchemasCommand);
	return jobIds;
}


/*
 * CreateJobIds returns a list of unique job ids that will be used
 * in mergeTasks.
 */
static List *
CreateJobIds(List *mergeTasks)
{
	ListCell *taskCell = NULL;
	List *jobIds = NIL;

	foreach(taskCell, mergeTasks)
	{
		Task *task = (Task *) lfirst(taskCell);
		if (!DoesJobIDExist(jobIds, task->jobId))
		{
			jobIds = lappend(jobIds, (void *) task->jobId);
		}
	}
	return jobIds;
}


/*
 * CreateSchemasOnAllWorkers creates schemas in all workers.
 */
static void
CreateSchemasOnAllWorkers(char *createSchemasCommand)
{
	List *commandList = list_make1(createSchemasCommand);

	SendCommandToAllWorkers(commandList);
}


/*
 * SendCommandToAllWorkers sends the given command to all workers in
 * a single transaction.
 */
static void
SendCommandToAllWorkers(List *commandList)
{
	ListCell *workerNodeCell = NULL;
	char *extensionOwner = CitusExtensionOwnerName();
	List *workerNodeList = ActiveReadableNodeList();

	foreach(workerNodeCell, workerNodeList)
	{
		WorkerNode *workerNode = (WorkerNode *) lfirst(workerNodeCell);
		SendCommandListToWorkerInSingleTransaction(workerNode->workerName,
												   workerNode->workerPort, extensionOwner,
												   commandList);
	}
}


/*
 * GenerateCreateSchemasCommand returns concatanated create schema commands.
 */
static char *
GenerateCreateSchemasCommand(List *jobIds)
{
	return GenerateCommand(jobIds, WORKER_CREATE_SCHEMA_QUERY);
}


/*
 * GenerateCommand returns concatenated commands with the given template
 * command for each job id from the given job ids. The returned command is
 * exactly list_length(jobIds) subcommands.
 *  E.g create_schema(jobId1); create_schema(jobId2); ...
 * This way we can send the command in just one latency to a worker.
 */
static char *
GenerateCommand(List *jobIds, char *templateCommand)
{
	StringInfo createSchemaCommand = makeStringInfo();
	ListCell *jobIdCell = NULL;

	foreach(jobIdCell, jobIds)
	{
		uint64 jobId = (uint64) lfirst(jobIdCell);
		appendStringInfo(createSchemaCommand, templateCommand, jobId);
	}
	return createSchemaCommand->data;
}


/*
 * DoesJobIDExist returns true if the given job id exists in the given list.
 */
static bool
DoesJobIDExist(List *jobIds, uint64 jobId)
{
	ListCell *jobIdCell = NULL;
	foreach(jobIdCell, jobIds)
	{
		uint64 curJobId = (uint64) lfirst(jobIdCell);
		if (curJobId == jobId)
		{
			return true;
		}
	}
	return false;
}


/*
 * CleanUpSchemas removes all the schemas that start with pg_
 * in every worker.
 */
void
CleanUpSchemas()
{
	List *commandList = list_make1(JOB_SCHEMA_CLEANUP);
	SendCommandToAllWorkers(commandList);
}


/*
 * RemoveTempJobDirs removes the temporary job directories that are
 * used for repartition queries for the given job ids.
 */
static void
RemoveTempJobDirs(List *jobIds)
{
	List *commandList = list_make1(GenerateDeleteJobsCommand(jobIds));
	SendCommandToAllWorkers(commandList);
}


/*
 * GenerateDeleteJobsCommand returns concatanated remove job dir commands.
 */
static char *
GenerateDeleteJobsCommand(List *jobIds)
{
	return GenerateCommand(jobIds, WORKER_DELETE_JOBDIR_QUERY);
}
