// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.lake.compaction;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.Tablet;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.Config;
import com.starrocks.common.DuplicatedRequestException;
import com.starrocks.common.LabelAlreadyUsedException;
import com.starrocks.common.MetaNotFoundException;
import com.starrocks.common.UserException;
import com.starrocks.common.util.Daemon;
import com.starrocks.lake.LakeTablet;
import com.starrocks.lake.Utils;
import com.starrocks.proto.CompactRequest;
import com.starrocks.proto.CompactResponse;
import com.starrocks.rpc.BrpcProxy;
import com.starrocks.rpc.LakeService;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.service.FrontendOptions;
import com.starrocks.system.Backend;
import com.starrocks.system.SystemInfoService;
import com.starrocks.transaction.BeginTransactionException;
import com.starrocks.transaction.GlobalTransactionMgr;
import com.starrocks.transaction.TabletCommitInfo;
import com.starrocks.transaction.TransactionState;
import com.starrocks.transaction.VisibleStateWaiter;
import org.apache.commons.collections.CollectionUtils;
import org.apache.commons.collections4.queue.CircularFifoQueue;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import javax.validation.constraints.NotNull;

public class CompactionScheduler extends Daemon {
    private static final Logger LOG = LogManager.getLogger(CompactionScheduler.class);
    private static final String HOST_NAME = FrontendOptions.getLocalHostAddress();
    private static final long LOOP_INTERVAL_MS = 500L;
    private static final long TXN_TIMEOUT_SECOND = 86400L;
    private static final long MIN_COMPACTION_INTERVAL_MS_ON_SUCCESS = 3000L;
    private static final long MIN_COMPACTION_INTERVAL_MS_ON_FAILURE = 6000L;
    private static final long PARTITION_CLEAN_INTERVAL_SECOND = 30;
    private final CompactionManager compactionManager;
    private final SystemInfoService systemInfoService;
    private final GlobalTransactionMgr transactionMgr;
    private final GlobalStateMgr stateMgr;
    private final ConcurrentHashMap<PartitionIdentifier, CompactionContext> runningCompactions;
    private final SynchronizedCircularQueue<CompactionRecord> history;
    private final SynchronizedCircularQueue<CompactionRecord> failHistory;
    private boolean finishedWaiting = false;
    private long waitTxnId = -1;
    private long lastPartitionCleanTime;

    CompactionScheduler(@NotNull CompactionManager compactionManager, @NotNull SystemInfoService systemInfoService,
                        @NotNull GlobalTransactionMgr transactionMgr, @NotNull GlobalStateMgr stateMgr) {
        super("COMPACTION_DISPATCH", LOOP_INTERVAL_MS);
        this.compactionManager = compactionManager;
        this.systemInfoService = systemInfoService;
        this.transactionMgr = transactionMgr;
        this.stateMgr = stateMgr;
        this.runningCompactions = new ConcurrentHashMap<>();
        this.lastPartitionCleanTime = System.currentTimeMillis();
        this.history = new SynchronizedCircularQueue<>(Config.lake_compaction_history_size);
        this.failHistory = new SynchronizedCircularQueue<>(Config.lake_compaction_fail_history_size);
    }

    @Override
    protected void runOneCycle() {
        cleanPartition();

        // Schedule compaction tasks only when this is a leader FE and all edit logs have finished replay.
        // In order to ensure that the input rowsets of compaction still exists when doing publishing version, it is
        // necessary to ensure that the compaction task of the same partition is executed serially, that is, the next
        // compaction task can be executed only after the status of the previous compaction task changes to visible or canceled.
        if (stateMgr.isLeader() && stateMgr.isReady() && allCommittedTransactionsBeforeRestartHaveFinished()) {
            schedule();
            history.changeMaxSize(Config.lake_compaction_history_size);
            failHistory.changeMaxSize(Config.lake_compaction_fail_history_size);
        }
    }

    // Returns true if all transactions committed before this restart have finished(i.e., of VISIBLE state).
    // Technically, we only need to wait for compaction transactions finished, but I don't want to check the
    // type of each transaction.
    private boolean allCommittedTransactionsBeforeRestartHaveFinished() {
        if (finishedWaiting) {
            return true;
        }
        // Note: must call getMinActiveTxnId() before getNextTransactionId(), otherwise if there are no running transactions
        // waitTxnId <= minActiveTxnId will always be false.
        long minActiveTxnId = transactionMgr.getMinActiveTxnId();
        if (waitTxnId < 0) {
            waitTxnId = transactionMgr.getTransactionIDGenerator().getNextTransactionId();
        }
        finishedWaiting = waitTxnId <= minActiveTxnId;
        return finishedWaiting;
    }

    private void schedule() {
        // Check whether there are completed compaction jobs.
        for (Iterator<Map.Entry<PartitionIdentifier, CompactionContext>> iterator = runningCompactions.entrySet().iterator();
                iterator.hasNext(); ) {
            Map.Entry<PartitionIdentifier, CompactionContext> entry = iterator.next();
            PartitionIdentifier partition = entry.getKey();
            CompactionContext context = entry.getValue();

            if (context.compactionFinishedOnBE() && !context.transactionHasCommitted()) {
                try {
                    commitCompaction(partition, context);
                } catch (Exception e) {
                    LOG.error("Fail to commit compaction. {} error={}", context.getDebugString(), e.getMessage());
                    iterator.remove();
                    context.setFinishTs(System.currentTimeMillis());
                    failHistory.offer(CompactionRecord.build(context, e.getMessage()));
                    compactionManager.enableCompactionAfter(partition, MIN_COMPACTION_INTERVAL_MS_ON_FAILURE);
                    try {
                        transactionMgr.abortTransaction(partition.getDbId(), context.getTxnId(), e.getMessage());
                    } catch (UserException ex) {
                        LOG.error("Fail to abort txn " + context.getTxnId(), ex);
                    }
                    continue;
                }
            }

            if (context.transactionHasCommitted() && context.waitTransactionVisible(100, TimeUnit.MILLISECONDS)) {
                iterator.remove();
                context.setFinishTs(System.currentTimeMillis());
                history.offer(CompactionRecord.build(context));
                long cost = context.getFinishTs() - context.getStartTs();
                if (cost >= /*60 minutes=*/3600000) {
                    LOG.info("Removed published compaction. {} cost={}s running={}", context.getDebugString(),
                            cost / 1000, runningCompactions.size());
                } else if (LOG.isDebugEnabled()) {
                    LOG.debug("Removed published compaction. {} cost={}s running={}", context.getDebugString(),
                            cost / 1000, runningCompactions.size());
                }
                compactionManager.enableCompactionAfter(partition, MIN_COMPACTION_INTERVAL_MS_ON_SUCCESS);
            }
        }

        // Create new compaction tasks.
        int index = 0;
        int compactionLimit = compactionTaskLimit();
        int numRunningTasks = runningCompactions.values().stream().mapToInt(CompactionContext::getNumCompactionTasks).sum();
        if (numRunningTasks >= compactionLimit) {
            return;
        }

        List<PartitionIdentifier> partitions = compactionManager.choosePartitionsToCompact(runningCompactions.keySet());
        while (numRunningTasks < compactionLimit && index < partitions.size()) {
            PartitionIdentifier partition = partitions.get(index++);
            CompactionContext context = startCompaction(partition);
            if (context == null) {
                continue;
            }
            numRunningTasks += context.getNumCompactionTasks();
            runningCompactions.put(partition, context);
            if (LOG.isDebugEnabled()) {
                LOG.debug("Created new compaction job. partition={} txnId={}", partition, context.getTxnId());
            }
        }
    }

    private int compactionTaskLimit() {
        if (Config.lake_compaction_max_tasks >= 0) {
            return Config.lake_compaction_max_tasks;
        }
        return systemInfoService.getAliveBackendNumber() * 16;
    }

    private void cleanPartition() {
        long now = System.currentTimeMillis();
        if (now - lastPartitionCleanTime >= PARTITION_CLEAN_INTERVAL_SECOND * 1000L) {
            compactionManager.getAllPartitions()
                    .stream()
                    .filter(p -> !isPartitionExist(p))
                    .filter(p -> !runningCompactions.containsKey(p)) // Ignore those partitions in runningCompactions
                    .forEach(compactionManager::removePartition);
            lastPartitionCleanTime = now;
        }
    }

    private boolean isPartitionExist(PartitionIdentifier partition) {
        Database db = stateMgr.getDb(partition.getDbId());
        if (db == null) {
            return false;
        }
        db.readLock();
        try {
            // lake table or lake materialized view
            OlapTable table = (OlapTable) db.getTable(partition.getTableId());
            return table != null && table.getPartition(partition.getPartitionId()) != null;
        } finally {
            db.readUnlock();
        }
    }

    private CompactionContext startCompaction(PartitionIdentifier partitionIdentifier) {
        Database db = stateMgr.getDb(partitionIdentifier.getDbId());
        if (db == null) {
            compactionManager.removePartition(partitionIdentifier);
            return null;
        }

        if (!db.tryReadLock(50, TimeUnit.MILLISECONDS)) {
            LOG.info("Skipped partition compaction due to get database lock timeout");
            compactionManager.enableCompactionAfter(partitionIdentifier, MIN_COMPACTION_INTERVAL_MS_ON_FAILURE);
            return null;
        }

        long txnId;
        long currentVersion;
        OlapTable table;
        Partition partition;
        Map<Long, List<Long>> beToTablets;

        try {
            // lake table or lake materialized view
            table = (OlapTable) db.getTable(partitionIdentifier.getTableId());
            // Compact a table of SCHEMA_CHANGE state does not make much sense, because the compacted data
            // will not be used after the schema change job finished.
            if (table != null && table.getState() == OlapTable.OlapTableState.SCHEMA_CHANGE) {
                compactionManager.enableCompactionAfter(partitionIdentifier, MIN_COMPACTION_INTERVAL_MS_ON_FAILURE);
                return null;
            }
            partition = (table != null) ? table.getPartition(partitionIdentifier.getPartitionId()) : null;
            if (partition == null) {
                compactionManager.removePartition(partitionIdentifier);
                return null;
            }

            currentVersion = partition.getVisibleVersion();

            beToTablets = collectPartitionTablets(partition);
            if (beToTablets.isEmpty()) {
                compactionManager.enableCompactionAfter(partitionIdentifier, MIN_COMPACTION_INTERVAL_MS_ON_FAILURE);
                return null;
            }

            // Note: call `beginTransaction()` in the scope of database reader lock to make sure no shadow index will
            // be added to this table(i.e., no schema change) before calling `beginTransaction()`.
            txnId = beginTransaction(partitionIdentifier);
        } catch (BeginTransactionException | AnalysisException | LabelAlreadyUsedException | DuplicatedRequestException e) {
            LOG.error("Fail to create transaction for compaction job. {}", e.getMessage());
            return null;
        } catch (Throwable e) {
            LOG.error("Unknown error: {}", e.getMessage());
            return null;
        } finally {
            db.readUnlock();
        }

        String partitionName = String.format("%s.%s.%s", db.getFullName(), table.getName(), partition.getName());
        CompactionContext context = new CompactionContext(partitionName, txnId, System.currentTimeMillis());
        context.setBeToTablets(beToTablets);

        long nextCompactionInterval = MIN_COMPACTION_INTERVAL_MS_ON_SUCCESS;
        try {
            List<Future<CompactResponse>> futures = compactTablets(currentVersion, beToTablets, txnId);
            context.setResponseList(futures);
            return context;
        } catch (Exception e) {
            LOG.error(e);
            nextCompactionInterval = MIN_COMPACTION_INTERVAL_MS_ON_FAILURE;
            abortTransactionIgnoreError(db.getId(), txnId, e.getMessage());
            context.setFinishTs(System.currentTimeMillis());
            failHistory.offer(CompactionRecord.build(context, e.getMessage()));
            return null;
        } finally {
            compactionManager.enableCompactionAfter(partitionIdentifier, nextCompactionInterval);
        }
    }

    @NotNull
    private List<Future<CompactResponse>> compactTablets(long currentVersion, Map<Long, List<Long>> beToTablets, long txnId)
            throws UserException {
        List<Future<CompactResponse>> futures = Lists.newArrayListWithCapacity(beToTablets.size());
        for (Map.Entry<Long, List<Long>> entry : beToTablets.entrySet()) {
            Backend backend = systemInfoService.getBackend(entry.getKey());
            if (backend == null) {
                throw new UserException("Backend " + entry.getKey() + " has been dropped");
            }
            CompactRequest request = new CompactRequest();
            request.tabletIds = entry.getValue();
            request.txnId = txnId;
            request.version = currentVersion;

            LakeService service = BrpcProxy.getLakeService(backend.getHost(), backend.getBrpcPort());
            futures.add(service.compact(request));
        }
        return futures;
    }

    @NotNull
    private Map<Long, List<Long>> collectPartitionTablets(Partition partition) {
        List<MaterializedIndex> visibleIndexes = partition.getMaterializedIndices(MaterializedIndex.IndexExtState.VISIBLE);
        Map<Long, List<Long>> beToTablets = new HashMap<>();
        for (MaterializedIndex index : visibleIndexes) {
            for (Tablet tablet : index.getTablets()) {
                Long beId = Utils.chooseBackend((LakeTablet) tablet);
                if (beId == null) {
                    beToTablets.clear();
                    return beToTablets;
                }
                beToTablets.computeIfAbsent(beId, k -> Lists.newArrayList()).add(tablet.getId());
            }
        }
        return beToTablets;
    }

    // REQUIRE: has acquired the exclusive lock of Database.
    private long beginTransaction(PartitionIdentifier partition)
            throws BeginTransactionException, AnalysisException, LabelAlreadyUsedException, DuplicatedRequestException {
        long dbId = partition.getDbId();
        long tableId = partition.getTableId();
        long partitionId = partition.getPartitionId();
        long currentTs = System.currentTimeMillis();
        TransactionState.LoadJobSourceType loadJobSourceType = TransactionState.LoadJobSourceType.LAKE_COMPACTION;
        TransactionState.TxnSourceType txnSourceType = TransactionState.TxnSourceType.FE;
        TransactionState.TxnCoordinator coordinator = new TransactionState.TxnCoordinator(txnSourceType, HOST_NAME);
        String label = String.format("COMPACTION_%d-%d-%d-%d", dbId, tableId, partitionId, currentTs);
        return transactionMgr.beginTransaction(dbId, Lists.newArrayList(tableId), label, coordinator,
                loadJobSourceType, TXN_TIMEOUT_SECOND);
    }

    private void commitCompaction(PartitionIdentifier partition, CompactionContext context)
            throws UserException, ExecutionException, InterruptedException {
        Preconditions.checkState(context.compactionFinishedOnBE());

        for (Future<CompactResponse> responseFuture : context.getResponseList()) {
            CompactResponse response = responseFuture.get();
            if (response != null && CollectionUtils.isNotEmpty(response.failedTablets)) {
                if (response.status != null && CollectionUtils.isNotEmpty(response.status.errorMsgs)) {
                    throw new UserException(response.status.errorMsgs.get(0));
                } else {
                    throw new UserException("Fail to compact tablet " + response.failedTablets.get(0));
                }
            }
        }

        List<TabletCommitInfo> commitInfoList = Lists.newArrayList();
        for (Map.Entry<Long, List<Long>> entry : context.getBeToTablets().entrySet()) {
            for (Long tabletId : entry.getValue()) {
                commitInfoList.add(new TabletCommitInfo(tabletId, entry.getKey()));
            }
        }

        Database db = stateMgr.getDb(partition.getDbId());
        if (db == null) {
            throw new MetaNotFoundException("database not exist");
        }
        if (LOG.isDebugEnabled()) {
            LOG.debug("Committing compaction transaction. partition={} txnId={}", partition, context.getTxnId());
        }

        VisibleStateWaiter waiter;
        db.writeLock();
        try {
            waiter = transactionMgr.commitTransaction(db.getId(), context.getTxnId(), commitInfoList, Lists.newArrayList());
        } finally {
            db.writeUnlock();
        }
        context.setVisibleStateWaiter(waiter);
        context.setCommitTs(System.currentTimeMillis());
    }

    private void abortTransactionIgnoreError(long dbId, long txnId, String reason) {
        try {
            transactionMgr.abortTransaction(dbId, txnId, reason);
        } catch (UserException ex) {
            LOG.error(ex);
        }
    }

    @NotNull
    List<CompactionRecord> getHistory() {
        ImmutableList.Builder<CompactionRecord> builder = ImmutableList.builder();
        history.forEach(builder::add);
        failHistory.forEach(builder::add);
        for (CompactionContext context : runningCompactions.values()) {
            builder.add(CompactionRecord.build(context));
        }
        return builder.build();
    }

    private static class SynchronizedCircularQueue<E> {
        private CircularFifoQueue<E> q;

        SynchronizedCircularQueue(int size) {
            q = new CircularFifoQueue<>(size);
        }

        synchronized void changeMaxSize(int newMaxSize) {
            if (newMaxSize == q.maxSize()) {
                return;
            }
            CircularFifoQueue<E> newQ = new CircularFifoQueue<>(newMaxSize);
            for (E e : q) {
                newQ.offer(e);
            }
            q = newQ;
        }

        synchronized void offer(E e) {
            q.offer(e);
        }

        synchronized void forEach(Consumer<? super E> consumer) {
            q.forEach(consumer);
        }
    }
}
