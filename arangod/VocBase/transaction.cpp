////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "transaction.h"
#include "Aql/QueryCache.h"
#include "Logger/Logger.h"
#include "Basics/Exceptions.h"
#include "VocBase/DatafileHelper.h"
#include "VocBase/collection.h"
#include "VocBase/document-collection.h"
#include "VocBase/server.h"
#include "VocBase/vocbase.h"
#include "Wal/DocumentOperation.h"
#include "Wal/LogfileManager.h"
#include "Utils/Transaction.h"

#ifdef ARANGODB_ENABLE_ROCKSDB
#include "Indexes/RocksDBFeature.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>
#include <rocksdb/utilities/transaction.h>
#endif

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE

#define LOG_TRX(trx, level)  \
  LOG(TRACE) << "trx #" << trx->_id << "." << level << " (" << StatusTransaction(trx->_status) << "): " 

#else

#define LOG_TRX(...) while (0) LOG(TRACE)

#endif

using namespace arangodb;
  
////////////////////////////////////////////////////////////////////////////////
/// @brief returns whether the collection is currently locked
////////////////////////////////////////////////////////////////////////////////

static inline bool IsLocked(TRI_transaction_collection_t const* trxCollection) {
  return (trxCollection->_lockType != TRI_TRANSACTION_NONE);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the logfile manager
////////////////////////////////////////////////////////////////////////////////

static inline arangodb::wal::LogfileManager* GetLogfileManager() {
  return arangodb::wal::LogfileManager::instance();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a transaction is read-only
////////////////////////////////////////////////////////////////////////////////

static inline bool IsReadOnlyTransaction(TRI_transaction_t const* trx) {
  return (trx->_type == TRI_TRANSACTION_READ);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a specific hint is set for the transaction
////////////////////////////////////////////////////////////////////////////////

static inline bool HasHint(TRI_transaction_t const* trx,
                           TRI_transaction_hint_e hint) {
  return ((trx->_hints & (TRI_transaction_hint_t)hint) != 0);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a transaction consists of a single operation
////////////////////////////////////////////////////////////////////////////////

static inline bool IsSingleOperationTransaction(TRI_transaction_t const* trx) {
  return HasHint(trx, TRI_TRANSACTION_HINT_SINGLE_OPERATION);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a marker needs to be written
////////////////////////////////////////////////////////////////////////////////

static inline bool NeedWriteMarker(TRI_transaction_t const* trx,
                                   bool isBeginMarker) {
  if (isBeginMarker) {
    return (!IsReadOnlyTransaction(trx) && !IsSingleOperationTransaction(trx));
  }

  return (trx->_nestingLevel == 0 && trx->_beginWritten &&
          !IsReadOnlyTransaction(trx) && !IsSingleOperationTransaction(trx));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief clear the query cache for all collections that were modified by
/// the transaction
////////////////////////////////////////////////////////////////////////////////

void ClearQueryCache(TRI_transaction_t* trx) {
  if (trx->_collections.empty()) {
    return;
  }

  try {
    std::vector<std::string> collections;
    for (auto& trxCollection : trx->_collections) {
      if (trxCollection->_accessType != TRI_TRANSACTION_WRITE ||
          trxCollection->_operations == nullptr ||
          trxCollection->_operations->empty()) {
        // we're only interested in collections that may have been modified
        continue;
      }

      collections.emplace_back(trxCollection->_collection->name());
    }

    if (!collections.empty()) {
      arangodb::aql::QueryCache::instance()->invalidate(trx->_vocbase,
                                                        collections);
    }
  } catch (...) {
    // in case something goes wrong, we have to remove all queries from the
    // cache
    arangodb::aql::QueryCache::instance()->invalidate(trx->_vocbase);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the status of the transaction as a string
////////////////////////////////////////////////////////////////////////////////

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
static char const* StatusTransaction(const TRI_transaction_status_e status) {
  switch (status) {
    case TRI_TRANSACTION_UNDEFINED:
      return "undefined";
    case TRI_TRANSACTION_CREATED:
      return "created";
    case TRI_TRANSACTION_RUNNING:
      return "running";
    case TRI_TRANSACTION_COMMITTED:
      return "committed";
    case TRI_TRANSACTION_ABORTED:
      return "aborted";
  }

  TRI_ASSERT(false);
  return "unknown";
}
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief free all operations for a transaction
////////////////////////////////////////////////////////////////////////////////

static void FreeOperations(TRI_transaction_t* trx) {
  bool const mustRollback = (trx->_status == TRI_TRANSACTION_ABORTED);
  bool const isSingleOperation = IsSingleOperationTransaction(trx);
      
  std::unordered_map<TRI_voc_fid_t, std::pair<int64_t, int64_t>> stats;

  for (auto& trxCollection : trx->_collections) {
    if (trxCollection->_operations == nullptr) {
      continue;
    }

    TRI_document_collection_t* document =
        trxCollection->_collection->_collection;

    if (mustRollback) {
      // revert all operations
      for (auto it = trxCollection->_operations->rbegin();
           it != trxCollection->_operations->rend(); ++it) {
        arangodb::wal::DocumentOperation* op = (*it);

        op->revert();
      }
    } else {
      // update datafile statistics for all operations
      // pair (number of dead markers, size of dead markers)
      stats.clear();

      for (auto it = trxCollection->_operations->rbegin();
           it != trxCollection->_operations->rend(); ++it) {
        arangodb::wal::DocumentOperation* op = (*it);

        if (op->type == TRI_VOC_DOCUMENT_OPERATION_UPDATE ||
            op->type == TRI_VOC_DOCUMENT_OPERATION_REPLACE ||
            op->type == TRI_VOC_DOCUMENT_OPERATION_REMOVE) {
          TRI_voc_fid_t fid = op->oldHeader.getFid();
          auto it2 = stats.find(fid);

          if (it2 == stats.end()) {
            stats.emplace(fid, std::make_pair(1, static_cast<int64_t>(op->oldHeader.alignedMarkerSize())));
          } else {
            (*it2).second.first++;
            (*it2).second.second += static_cast<int64_t>(op->oldHeader.alignedMarkerSize());
          }
        }
      }

      // now update the stats for all datafiles of the collection in one go
      for (auto const& it : stats) {
        document->_datafileStatistics.increaseDead(it.first, it.second.first,
                                                   it.second.second);
      }
    }

    for (auto it = trxCollection->_operations->rbegin();
         it != trxCollection->_operations->rend(); ++it) {
      arangodb::wal::DocumentOperation* op = (*it);

      delete op;
    }

    if (mustRollback) {
      document->_info.setRevision(trxCollection->_originalRevision, true);
    } else if (!document->_info.isVolatile() && !isSingleOperation) {
      // only count logfileEntries if the collection is durable
      document->_uncollectedLogfileEntries +=
          trxCollection->_operations->size();
    }

    delete trxCollection->_operations;
    trxCollection->_operations = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find a collection in the transaction's list of collections
////////////////////////////////////////////////////////////////////////////////

static TRI_transaction_collection_t* FindCollection(
    TRI_transaction_t const* trx, TRI_voc_cid_t cid,
    size_t* position) {

  size_t const n = trx->_collections.size();
  size_t i;

  for (i = 0; i < n; ++i) {
    auto trxCollection = trx->_collections.at(i);

    if (cid < trxCollection->_cid) {
      // collection not found
      break;
    }

    if (cid == trxCollection->_cid) {
      // found
      return trxCollection;
    }
    // next
  }

  if (position != nullptr) {
    // update the insert position if required
    *position = i;
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a transaction collection container
////////////////////////////////////////////////////////////////////////////////

static TRI_transaction_collection_t* CreateCollection(
    TRI_transaction_t* trx, TRI_voc_cid_t cid,
    TRI_transaction_type_e accessType, int nestingLevel) {
  TRI_transaction_collection_t* trxCollection =
      static_cast<TRI_transaction_collection_t*>(TRI_Allocate(
          TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_transaction_collection_t), false));

  if (trxCollection == nullptr) {
    // OOM
    return nullptr;
  }

  // initialize collection properties
  trxCollection->_transaction = trx;
  trxCollection->_cid = cid;
  trxCollection->_accessType = accessType;
  trxCollection->_nestingLevel = nestingLevel;
  trxCollection->_collection = nullptr;
  trxCollection->_operations = nullptr;
  trxCollection->_originalRevision = 0;
  trxCollection->_lockType = TRI_TRANSACTION_NONE;
  trxCollection->_compactionLocked = false;
  trxCollection->_waitForSync = false;

  return trxCollection;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free a transaction collection container
////////////////////////////////////////////////////////////////////////////////

static void FreeCollection(TRI_transaction_collection_t* trxCollection) {
  TRI_ASSERT(trxCollection != nullptr);

  TRI_Free(TRI_UNKNOWN_MEM_ZONE, trxCollection);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief lock a collection
////////////////////////////////////////////////////////////////////////////////

static int LockCollection(TRI_transaction_collection_t* trxCollection,
                          TRI_transaction_type_e type, int nestingLevel) {
  TRI_ASSERT(trxCollection != nullptr);

  TRI_transaction_t* trx = trxCollection->_transaction;

  if (HasHint(trx, TRI_TRANSACTION_HINT_LOCK_NEVER)) {
    // never lock
    return TRI_ERROR_NO_ERROR;
  }

  TRI_ASSERT(trxCollection->_collection != nullptr);

  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(trxCollection->_collection->_name);
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "LockCollection blocked: " << collName << std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  TRI_ASSERT(trxCollection->_collection->_collection != nullptr);
  TRI_ASSERT(!IsLocked(trxCollection));

  TRI_document_collection_t* document = trxCollection->_collection->_collection;
  uint64_t timeout = trx->_timeout;
  if (HasHint(trxCollection->_transaction, TRI_TRANSACTION_HINT_TRY_LOCK)) {
    // give up if we cannot acquire the lock instantly
    timeout = 1 * 100;
  }

  int res;
  if (type == TRI_TRANSACTION_READ) {
    LOG_TRX(trx, nestingLevel) << "read-locking collection " << trxCollection->_cid;
    res = document->beginReadTimed(timeout,
                                   TRI_TRANSACTION_DEFAULT_SLEEP_DURATION);
  } else {
    LOG_TRX(trx, nestingLevel) << "write-locking collection " << trxCollection->_cid;
    res = document->beginWriteTimed(timeout,
                                    TRI_TRANSACTION_DEFAULT_SLEEP_DURATION);
  }

  if (res == TRI_ERROR_NO_ERROR) {
    trxCollection->_lockType = type;
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief unlock a collection
////////////////////////////////////////////////////////////////////////////////

static int UnlockCollection(TRI_transaction_collection_t* trxCollection,
                            TRI_transaction_type_e type, int nestingLevel) {
  TRI_ASSERT(trxCollection != nullptr);

  if (HasHint(trxCollection->_transaction, TRI_TRANSACTION_HINT_LOCK_NEVER)) {
    // never unlock
    return TRI_ERROR_NO_ERROR;
  }

  TRI_ASSERT(trxCollection->_collection != nullptr);

  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(trxCollection->_collection->_name);
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "UnlockCollection blocked: " << collName << std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  TRI_ASSERT(trxCollection->_collection->_collection != nullptr);
  TRI_ASSERT(IsLocked(trxCollection));

  TRI_document_collection_t* document = trxCollection->_collection->_collection;

  if (trxCollection->_nestingLevel < nestingLevel) {
    // only process our own collections
    return TRI_ERROR_NO_ERROR;
  }

  if (type == TRI_TRANSACTION_READ &&
      trxCollection->_lockType == TRI_TRANSACTION_WRITE) {
    // do not remove a write-lock if a read-unlock was requested!
    return TRI_ERROR_NO_ERROR;
  } else if (type == TRI_TRANSACTION_WRITE &&
             trxCollection->_lockType == TRI_TRANSACTION_READ) {
    // we should never try to write-unlock a collection that we have only
    // read-locked
    LOG(ERR) << "logic error in UnlockCollection";
    TRI_ASSERT(false);
    return TRI_ERROR_INTERNAL;
  }

  if (trxCollection->_lockType == TRI_TRANSACTION_READ) {
    LOG_TRX(trxCollection->_transaction, nestingLevel) << "read-unlocking collection " << trxCollection->_cid;
    document->endRead();
  } else {
    LOG_TRX(trxCollection->_transaction, nestingLevel) << "write-unlocking collection " << trxCollection->_cid;
    document->endWrite();
  }

  trxCollection->_lockType = TRI_TRANSACTION_NONE;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief use all participating collections of a transaction
////////////////////////////////////////////////////////////////////////////////

static int UseCollections(TRI_transaction_t* trx, int nestingLevel) {
  // process collections in forward order
  for (auto& trxCollection : trx->_collections) {
    if (trxCollection->_nestingLevel != nestingLevel) {
      // only process our own collections
      continue;
    }

    if (trxCollection->_collection == nullptr) {
      // open the collection
      if (!HasHint(trx, TRI_TRANSACTION_HINT_LOCK_NEVER) &&
          !HasHint(trx, TRI_TRANSACTION_HINT_NO_USAGE_LOCK)) {
        // use and usage-lock
        TRI_vocbase_col_status_e status;
        LOG_TRX(trx, nestingLevel) << "using collection " << trxCollection->_cid;
        trxCollection->_collection = TRI_UseCollectionByIdVocBase(
            trx->_vocbase, trxCollection->_cid, status);
      } else {
        // use without usage-lock (lock already set externally)
        trxCollection->_collection =
            TRI_LookupCollectionByIdVocBase(trx->_vocbase, trxCollection->_cid);
      }

      if (trxCollection->_collection == nullptr ||
          trxCollection->_collection->_collection == nullptr) {
        // something went wrong
        return TRI_errno();
      }

      if (trxCollection->_accessType == TRI_TRANSACTION_WRITE &&
          TRI_GetOperationModeServer() == TRI_VOCBASE_MODE_NO_CREATE &&
          !TRI_IsSystemNameCollection(
              trxCollection->_collection->_collection->_info.namec_str())) {
        return TRI_ERROR_ARANGO_READ_ONLY;
      }

      // store the waitForSync property
      trxCollection->_waitForSync =
          trxCollection->_collection->_collection->_info.waitForSync();
    }

    TRI_ASSERT(trxCollection->_collection != nullptr);
    TRI_ASSERT(trxCollection->_collection->_collection != nullptr);

    if (nestingLevel == 0 &&
        trxCollection->_accessType == TRI_TRANSACTION_WRITE) {
      // read-lock the compaction lock
      if (!HasHint(trx, TRI_TRANSACTION_HINT_NO_COMPACTION_LOCK)) {
        if (!trxCollection->_compactionLocked) {
          trxCollection->_collection->_collection->_compactionLock.readLock();
          trxCollection->_compactionLocked = true;
        }
      }
    }

    if (trxCollection->_accessType == TRI_TRANSACTION_WRITE &&
        trxCollection->_originalRevision == 0) {
      // store original revision at transaction start
      trxCollection->_originalRevision =
          trxCollection->_collection->_collection->_info.revision();
    }

    bool shouldLock = HasHint(trx, TRI_TRANSACTION_HINT_LOCK_ENTIRELY);

    if (!shouldLock) {
      shouldLock = (trxCollection->_accessType == TRI_TRANSACTION_WRITE) &&
                   (!IsSingleOperationTransaction(trx));
    }

    if (shouldLock && !IsLocked(trxCollection)) {
      // r/w lock the collection
      int res = LockCollection(trxCollection, trxCollection->_accessType,
                               nestingLevel);

      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief release collection locks for a transaction
////////////////////////////////////////////////////////////////////////////////

static int UnuseCollections(TRI_transaction_t* trx, int nestingLevel) {
  int res = TRI_ERROR_NO_ERROR;

  // process collections in reverse order
  for (auto it = trx->_collections.rbegin(); it != trx->_collections.rend(); ++it) {
    TRI_transaction_collection_t* trxCollection = (*it);

    if (IsLocked(trxCollection) &&
        (nestingLevel == 0 || trxCollection->_nestingLevel == nestingLevel)) {
      // unlock our own r/w locks
      UnlockCollection(trxCollection, trxCollection->_accessType, nestingLevel);
    }

    // the top level transaction releases all collections
    if (nestingLevel == 0 && trxCollection->_collection != nullptr) {
      if (!HasHint(trx, TRI_TRANSACTION_HINT_NO_COMPACTION_LOCK)) {
        if (trxCollection->_accessType == TRI_TRANSACTION_WRITE &&
            trxCollection->_compactionLocked) {
          // read-unlock the compaction lock
          trxCollection->_collection->_collection->_compactionLock.unlock();
          trxCollection->_compactionLocked = false;
        }
      }

      trxCollection->_lockType = TRI_TRANSACTION_NONE;
    }
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief release collection locks for a transaction
////////////////////////////////////////////////////////////////////////////////

static int ReleaseCollections(TRI_transaction_t* trx, int nestingLevel) {
  TRI_ASSERT(nestingLevel == 0);
  if (HasHint(trx, TRI_TRANSACTION_HINT_LOCK_NEVER) ||
      HasHint(trx, TRI_TRANSACTION_HINT_NO_USAGE_LOCK)) {
    return TRI_ERROR_NO_ERROR;
  }

  // process collections in reverse order
  for (auto it = trx->_collections.rbegin(); it != trx->_collections.rend(); ++it) {
    TRI_transaction_collection_t* trxCollection = (*it);

    // the top level transaction releases all collections
    if (trxCollection->_collection != nullptr) {
      // unuse collection, remove usage-lock
      LOG_TRX(trx, nestingLevel) << "unusing collection " << trxCollection->_cid;

      TRI_ReleaseCollectionVocBase(trx->_vocbase, trxCollection->_collection);
      trxCollection->_collection = nullptr;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write WAL begin marker
////////////////////////////////////////////////////////////////////////////////

static int WriteBeginMarker(TRI_transaction_t* trx) {
  if (!NeedWriteMarker(trx, true)) {
    return TRI_ERROR_NO_ERROR;
  }

  if (HasHint(trx, TRI_TRANSACTION_HINT_NO_BEGIN_MARKER)) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_IF_FAILURE("TransactionWriteBeginMarker") { return TRI_ERROR_DEBUG; }

  TRI_ASSERT(!trx->_beginWritten);

  int res;

  try {
    arangodb::wal::TransactionMarker marker(TRI_DF_MARKER_VPACK_BEGIN_TRANSACTION, trx->_vocbase->_id, trx->_id);
    res = GetLogfileManager()->allocateAndWrite(marker, false).errorCode;

    if (res == TRI_ERROR_NO_ERROR) {
      trx->_beginWritten = true;
    }
  } catch (arangodb::basics::Exception const& ex) {
    res = ex.code();
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(WARN) << "could not save transaction begin marker in log: " << TRI_errno_string(res);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write WAL abort marker
////////////////////////////////////////////////////////////////////////////////

static int WriteAbortMarker(TRI_transaction_t* trx) {
  if (!NeedWriteMarker(trx, false)) {
    return TRI_ERROR_NO_ERROR;
  }

  if (HasHint(trx, TRI_TRANSACTION_HINT_NO_ABORT_MARKER)) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_ASSERT(trx->_beginWritten);

  TRI_IF_FAILURE("TransactionWriteAbortMarker") { return TRI_ERROR_DEBUG; }

  int res;

  try {
    arangodb::wal::TransactionMarker marker(TRI_DF_MARKER_VPACK_ABORT_TRANSACTION, trx->_vocbase->_id, trx->_id);
    res = GetLogfileManager()->allocateAndWrite(marker, false).errorCode;
  } catch (arangodb::basics::Exception const& ex) {
    res = ex.code();
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(WARN) << "could not save transaction abort marker in log: " << TRI_errno_string(res);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write WAL commit marker
////////////////////////////////////////////////////////////////////////////////

static int WriteCommitMarker(TRI_transaction_t* trx) {
  if (!NeedWriteMarker(trx, false)) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_IF_FAILURE("TransactionWriteCommitMarker") { return TRI_ERROR_DEBUG; }

  TRI_ASSERT(trx->_beginWritten);

  int res;

  try {
    arangodb::wal::TransactionMarker marker(TRI_DF_MARKER_VPACK_COMMIT_TRANSACTION, trx->_vocbase->_id, trx->_id);
    res = GetLogfileManager()->allocateAndWrite(marker, trx->_waitForSync).errorCode;
    
    TRI_IF_FAILURE("TransactionWriteCommitMarkerSegfault") { 
      TRI_SegfaultDebugging("crashing on commit");
    }
#ifdef ARANGODB_ENABLE_ROCKSDB

    TRI_IF_FAILURE("TransactionWriteCommitMarkerNoRocksSync") { return TRI_ERROR_NO_ERROR; }

    if (trx->_waitForSync) {
      // also sync RocksDB WAL
      RocksDBFeature::syncWal();
    }
#endif
  } catch (arangodb::basics::Exception const& ex) {
    res = ex.code();
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(WARN) << "could not save transaction commit marker in log: " << TRI_errno_string(res);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief update the status of a transaction
////////////////////////////////////////////////////////////////////////////////

static void UpdateTransactionStatus(TRI_transaction_t* const trx,
                                    TRI_transaction_status_e status) {
  TRI_ASSERT(trx->_status == TRI_TRANSACTION_CREATED ||
             trx->_status == TRI_TRANSACTION_RUNNING);

  if (trx->_status == TRI_TRANSACTION_CREATED) {
    TRI_ASSERT(status == TRI_TRANSACTION_RUNNING ||
               status == TRI_TRANSACTION_ABORTED);
  } else if (trx->_status == TRI_TRANSACTION_RUNNING) {
    TRI_ASSERT(status == TRI_TRANSACTION_COMMITTED ||
               status == TRI_TRANSACTION_ABORTED);
  }

  trx->_status = status;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the transaction type from a string
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_type_e TRI_GetTransactionTypeFromStr(char const* s) {
  if (strcmp(s, "read") == 0) {
    return TRI_TRANSACTION_READ;
  } 
  if (strcmp(s, "write") == 0) {
    return TRI_TRANSACTION_WRITE;
  } 
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "invalid transaction type");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection from a transaction
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_collection_t* TRI_GetCollectionTransaction(
    TRI_transaction_t const* trx, TRI_voc_cid_t cid,
    TRI_transaction_type_e accessType) {
  TRI_ASSERT(trx->_status == TRI_TRANSACTION_CREATED ||
             trx->_status == TRI_TRANSACTION_RUNNING);

  TRI_transaction_collection_t* trxCollection =
      FindCollection(trx, cid, nullptr);

  if (trxCollection == nullptr) {
    // not found
    return nullptr;
  }

  if (trxCollection->_collection == nullptr) {
    if (!HasHint(trx, TRI_TRANSACTION_HINT_LOCK_NEVER) ||
        !HasHint(trx, TRI_TRANSACTION_HINT_NO_USAGE_LOCK)) {
      // not opened. probably a mistake made by the caller
      return nullptr;
    }
    // ok
  }

  // check if access type matches
  if (accessType == TRI_TRANSACTION_WRITE &&
      trxCollection->_accessType == TRI_TRANSACTION_READ) {
    // type doesn't match. probably also a mistake by the caller
    return nullptr;
  }

  return trxCollection;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_AddCollectionTransaction(TRI_transaction_t* trx, TRI_voc_cid_t cid,
                                 TRI_transaction_type_e accessType,
                                 int nestingLevel, bool force,
                                 bool allowImplicitCollections) {
  LOG_TRX(trx, nestingLevel) << "adding collection " << cid;

  allowImplicitCollections &= trx->_allowImplicit;

  // LOG(TRACE) << "cid: " << cid 
  //            << ", accessType: " << accessType 
  //            << ", nestingLevel: " << nestingLevel 
  //            << ", force: " << force 
  //            << ", allowImplicitCollections: " << allowImplicitCollections;
  
  // upgrade transaction type if required
  if (nestingLevel == 0) {
    if (!force) {
      TRI_ASSERT(trx->_status == TRI_TRANSACTION_CREATED);
    }

    if (accessType == TRI_TRANSACTION_WRITE &&
        trx->_type == TRI_TRANSACTION_READ) {
      // if one collection is written to, the whole transaction becomes a
      // write-transaction
      trx->_type = TRI_TRANSACTION_WRITE;
    }
  }

  // check if we already have got this collection in the _collections vector
  size_t position = 0;
  TRI_transaction_collection_t* trxCollection =
      FindCollection(trx, cid, &position);
  
  if (trxCollection != nullptr) {
    // collection is already contained in vector

    if (accessType == TRI_TRANSACTION_WRITE &&
        trxCollection->_accessType != accessType) {
      if (nestingLevel > 0) {
        // trying to write access a collection that is only marked with
        // read-access
        return TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION;
      }

      TRI_ASSERT(nestingLevel == 0);

      // upgrade collection type to write-access
      trxCollection->_accessType = TRI_TRANSACTION_WRITE;
    }

    if (nestingLevel < trxCollection->_nestingLevel) {
      trxCollection->_nestingLevel = nestingLevel;
    }

    // all correct
    return TRI_ERROR_NO_ERROR;
  }

  // collection not found.

  if (nestingLevel > 0 && accessType == TRI_TRANSACTION_WRITE) {
    // trying to write access a collection in an embedded transaction
    return TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION;
  }

  if (accessType == TRI_TRANSACTION_READ && !allowImplicitCollections) {
    return TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION;
  }
  
  // collection was not contained. now create and insert it
  trxCollection = CreateCollection(trx, cid, accessType, nestingLevel);

  if (trxCollection == nullptr) {
    // out of memory
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  // insert collection at the correct position
  try {
    trx->_collections.insert(trx->_collections.begin() + position, trxCollection);
  } catch (...) {
    FreeCollection(trxCollection);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief make sure all declared collections are used & locked
////////////////////////////////////////////////////////////////////////////////

int TRI_EnsureCollectionsTransaction(TRI_transaction_t* trx, int nestingLevel) {
  return UseCollections(trx, nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief request a lock for a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_LockCollectionTransaction(TRI_transaction_collection_t* trxCollection,
                                  TRI_transaction_type_e accessType,
                                  int nestingLevel) {
  if (accessType == TRI_TRANSACTION_WRITE &&
      trxCollection->_accessType != TRI_TRANSACTION_WRITE) {
    // wrong lock type
    return TRI_ERROR_INTERNAL;
  }

  if (IsLocked(trxCollection)) {
    // already locked
    return TRI_ERROR_NO_ERROR;
  }

  return LockCollection(trxCollection, accessType, nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief request an unlock for a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_UnlockCollectionTransaction(TRI_transaction_collection_t* trxCollection,
                                    TRI_transaction_type_e accessType,
                                    int nestingLevel) {
  if (accessType == TRI_TRANSACTION_WRITE &&
      trxCollection->_accessType != TRI_TRANSACTION_WRITE) {
    // wrong lock type: write-unlock requested but collection is read-only
    return TRI_ERROR_INTERNAL;
  }

  if (!IsLocked(trxCollection)) {
    // already unlocked
    return TRI_ERROR_NO_ERROR;
  }

  return UnlockCollection(trxCollection, accessType, nestingLevel);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a collection is locked in a transaction
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsLockedCollectionTransaction(
    TRI_transaction_collection_t const* trxCollection,
    TRI_transaction_type_e accessType, int nestingLevel) {
  TRI_ASSERT(trxCollection != nullptr);

  if (accessType == TRI_TRANSACTION_WRITE &&
      trxCollection->_accessType != TRI_TRANSACTION_WRITE) {
    // wrong lock type
    LOG(WARN) << "logic error. checking wrong lock type";
    return false;
  }

  return IsLocked(trxCollection);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a collection is locked in a transaction
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsLockedCollectionTransaction(
    TRI_transaction_collection_t const* trxCollection) {
  TRI_ASSERT(trxCollection != nullptr);
  return IsLocked(trxCollection);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check whether a collection is used in a transaction
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsContainedCollectionTransaction(TRI_transaction_t* trx,
                                          TRI_voc_cid_t cid) {
  for (auto& trxCollection : trx->_collections) {
    if (trxCollection->_cid == cid) {
      return true;
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a WAL operation for a transaction collection
////////////////////////////////////////////////////////////////////////////////

int TRI_AddOperationTransaction(TRI_transaction_t* trx,
                                arangodb::wal::DocumentOperation& operation,
                                bool& waitForSync) {
  TRI_ASSERT(operation.header != nullptr);

  TRI_document_collection_t* document = operation.document;
  bool const isSingleOperationTransaction = IsSingleOperationTransaction(trx);

  if (HasHint(trx, TRI_TRANSACTION_HINT_RECOVERY)) {
    // turn off all waitForSync operations during recovery
    waitForSync = false;
  } else if (!waitForSync) {
    // upgrade the info for the transaction based on the collection's settings
    waitForSync |= document->_info.waitForSync();
  }

  if (waitForSync) {
    trx->_waitForSync = true;
  }

  TRI_IF_FAILURE("TransactionOperationNoSlot") { return TRI_ERROR_DEBUG; }

  TRI_IF_FAILURE("TransactionOperationNoSlotExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (!isSingleOperationTransaction && !trx->_beginWritten) {
    int res = WriteBeginMarker(trx);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  TRI_voc_fid_t fid = 0;
  void const* position = nullptr;

  if (operation.marker->fid() == 0) {
    // this is a "real" marker that must be written into the logfiles
    // just append it to the WAL:

    // we only need to set waitForSync to true here if waitForSync was requested
    // for the operation AND the operation is a standalone operation. In case the
    // operation belongs to a transaction, the transaction's commit marker will
    // be written with waitForSync, and we don't need to request a sync ourselves
    bool const localWaitForSync = (isSingleOperationTransaction && waitForSync);

    // never wait until our marker was synced, even when an operation was tagged
    // waitForSync=true. this is still safe because inside a transaction, the final
    // commit marker will be written with waitForSync=true then, and in a standalone
    // operation the transaction will wait until everything was synced before returning
    // to the caller
    bool const waitForTick = false;

    // we should wake up the synchronizer in case this is a single operation
    //
    bool const wakeUpSynchronizer = isSingleOperationTransaction;

    arangodb::wal::SlotInfoCopy slotInfo =
        arangodb::wal::LogfileManager::instance()->allocateAndWrite(
            trx->_vocbase->_id, document->_info.id(), 
            operation.marker, wakeUpSynchronizer,
            localWaitForSync, waitForTick);
    if (slotInfo.errorCode != TRI_ERROR_NO_ERROR) {
      // some error occurred
      return slotInfo.errorCode;
    }
#ifdef ARANGODB_ENABLE_ROCKSDB
    if (localWaitForSync) {
      // also sync RocksDB WAL
      RocksDBFeature::syncWal();
    }
#endif
    operation.tick = slotInfo.tick;
    fid = slotInfo.logfileId;
    position = slotInfo.mem;
  } else {
    // this is an envelope marker that has been written to the logfiles before.
    // avoid writing it again!
    fid = operation.marker->fid();
    position = static_cast<wal::MarkerEnvelope const*>(operation.marker)->mem();
  }

  TRI_ASSERT(fid > 0);
  TRI_ASSERT(position != nullptr);

  if (operation.type == TRI_VOC_DOCUMENT_OPERATION_INSERT ||
      operation.type == TRI_VOC_DOCUMENT_OPERATION_UPDATE ||
      operation.type == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
    // adjust the data position in the header
    operation.header->setVPackFromMarker(reinterpret_cast<TRI_df_marker_t const*>(position)); 
  }

  TRI_IF_FAILURE("TransactionOperationAfterAdjust") { return TRI_ERROR_DEBUG; }

  // set header file id
  TRI_ASSERT(fid > 0);
  operation.header->setFid(fid, true); // always in WAL

  if (isSingleOperationTransaction) {
    // operation is directly executed
#ifdef ARANGODB_ENABLE_ROCKSDB
    if (trx->_rocksTransaction != nullptr) {
      auto status = trx->_rocksTransaction->Commit();

      if (!status.ok()) { 
        // TODO: what to do here?
      }
    }
#endif
    operation.handle();

    arangodb::aql::QueryCache::instance()->invalidate(
        trx->_vocbase, document->_info.namec_str());

    ++document->_uncollectedLogfileEntries;

    if (operation.type == TRI_VOC_DOCUMENT_OPERATION_UPDATE ||
        operation.type == TRI_VOC_DOCUMENT_OPERATION_REPLACE ||
        operation.type == TRI_VOC_DOCUMENT_OPERATION_REMOVE) {
      // update datafile statistics for the old header
      document->_datafileStatistics.increaseDead(
          operation.oldHeader.getFid(), 1, static_cast<int64_t>(operation.oldHeader.alignedMarkerSize()));
    }
  } else {
    // operation is buffered and might be rolled back
    TRI_transaction_collection_t* trxCollection = TRI_GetCollectionTransaction(
        trx, document->_info.id(), TRI_TRANSACTION_WRITE);
    if (trxCollection->_operations == nullptr) {
      trxCollection->_operations = new std::vector<arangodb::wal::DocumentOperation*>;
      trxCollection->_operations->reserve(16);
      trx->_hasOperations = true;
    } else {
      // reserve space for one more element so the push_back below does not fail
      size_t oldSize = trxCollection->_operations->size();
      if (oldSize + 1 >= trxCollection->_operations->capacity()) {
        // double the size
        trxCollection->_operations->reserve((oldSize + 1) * 2);
      }
    }
    
    TRI_IF_FAILURE("TransactionOperationPushBack") {
      // test what happens if reserve above failed
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG); 
    }

    arangodb::wal::DocumentOperation* copy = operation.swap();
    
    // should not fail because we reserved enough room above 
    trxCollection->_operations->push_back(copy);
    copy->handle();
  }

  document->setLastRevision(operation.rid, false);

  TRI_IF_FAILURE("TransactionOperationAtEnd") { return TRI_ERROR_DEBUG; }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_BeginTransaction(TRI_transaction_t* trx, TRI_transaction_hint_t hints,
                         int nestingLevel) {
  LOG_TRX(trx, nestingLevel) << "beginning " << (trx->_type == TRI_TRANSACTION_READ ? "read" : "write") << " transaction";

  if (nestingLevel == 0) {
    TRI_ASSERT(trx->_status == TRI_TRANSACTION_CREATED);

    auto logfileManager = arangodb::wal::LogfileManager::instance();

    if (!HasHint(trx, TRI_TRANSACTION_HINT_NO_THROTTLING) &&
        trx->_type == TRI_TRANSACTION_WRITE &&
        logfileManager->canBeThrottled()) {
      // write-throttling?
      static uint64_t const WaitTime = 50000;
      uint64_t const maxIterations =
          logfileManager->maxThrottleWait() / (WaitTime / 1000);
      uint64_t iterations = 0;

      while (logfileManager->isThrottled()) {
        if (++iterations == maxIterations) {
          return TRI_ERROR_ARANGO_WRITE_THROTTLE_TIMEOUT;
        }

        usleep(WaitTime);
      }
    }

    // set hints
    trx->_hints = hints;

    // get a new id
    trx->_id = TRI_NewTickServer();

    // register a protector
    int res = logfileManager->registerTransaction(trx->_id);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  
  } else {
    TRI_ASSERT(trx->_status == TRI_TRANSACTION_RUNNING);
  }

  int res = UseCollections(trx, nestingLevel);

  if (res == TRI_ERROR_NO_ERROR) {
    // all valid
    if (nestingLevel == 0) {
      UpdateTransactionStatus(trx, TRI_TRANSACTION_RUNNING);

      // defer writing of the begin marker until necessary!
    }
  } else {
    // something is wrong
    if (nestingLevel == 0) {
      UpdateTransactionStatus(trx, TRI_TRANSACTION_ABORTED);
    }

    // free what we have got so far
    UnuseCollections(trx, nestingLevel);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief commit a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_CommitTransaction(TRI_transaction_t* trx, int nestingLevel) {
  LOG_TRX(trx, nestingLevel) << "committing " << (trx->_type == TRI_TRANSACTION_READ ? "read" : "write") << " transaction";

  TRI_ASSERT(trx->_status == TRI_TRANSACTION_RUNNING);

  int res = TRI_ERROR_NO_ERROR;

  if (nestingLevel == 0) {
#ifdef ARANGODB_ENABLE_ROCKSDB
    if (trx->_rocksTransaction != nullptr) {
      auto status = trx->_rocksTransaction->Commit();

      if (!status.ok()) {
        res = TRI_ERROR_INTERNAL;
        TRI_AbortTransaction(trx, nestingLevel);
        return res;
      }
    }
#endif

    res = WriteCommitMarker(trx);

    if (res != TRI_ERROR_NO_ERROR) {
      // TODO: revert rocks transaction somehow
      TRI_AbortTransaction(trx, nestingLevel);

      // return original error
      return res;
    }

    UpdateTransactionStatus(trx, TRI_TRANSACTION_COMMITTED);

    // if a write query, clear the query cache for the participating collections
    if (trx->_type == TRI_TRANSACTION_WRITE && 
        !trx->_collections.empty() &&
        arangodb::aql::QueryCache::instance()->mayBeActive()) {
      ClearQueryCache(trx);
    }

    FreeOperations(trx);
  }

  UnuseCollections(trx, nestingLevel);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief abort and rollback a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_AbortTransaction(TRI_transaction_t* trx, int nestingLevel) {
  LOG_TRX(trx, nestingLevel) << "aborting " << (trx->_type == TRI_TRANSACTION_READ ? "read" : "write") << " transaction";

  TRI_ASSERT(trx->_status == TRI_TRANSACTION_RUNNING);

  int res = TRI_ERROR_NO_ERROR;

  if (nestingLevel == 0) {
    res = WriteAbortMarker(trx);

    UpdateTransactionStatus(trx, TRI_TRANSACTION_ABORTED);

    FreeOperations(trx);
  }

  UnuseCollections(trx, nestingLevel);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a transaction consists of a single operation
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsSingleOperationTransaction(TRI_transaction_t const* trx) {
  return HasHint(trx, TRI_TRANSACTION_HINT_SINGLE_OPERATION);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief transaction type
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_t::TRI_transaction_t(TRI_vocbase_t* vocbase, double timeout, bool waitForSync)
    : _vocbase(vocbase), 
      _id(0), 
      _type(TRI_TRANSACTION_READ),
      _status(TRI_TRANSACTION_CREATED),
      _arena(),
      _collections{_arena}, // assign arena to vector 
#ifdef ARANGODB_ENABLE_ROCKSDB
      _rocksTransaction(nullptr),
#endif
      _hints(0),
      _nestingLevel(0), 
      _allowImplicit(true),
      _hasOperations(false), 
      _waitForSync(waitForSync),
      _beginWritten(false), 
      _timeout(TRI_TRANSACTION_DEFAULT_LOCK_TIMEOUT) {
  
  if (timeout > 0.0) {
    _timeout = (uint64_t)(timeout * 1000000.0);
  } else if (timeout == 0.0) {
    _timeout = static_cast<uint64_t>(0);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free a transaction container
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_t::~TRI_transaction_t() {
  if (_status == TRI_TRANSACTION_RUNNING) {
    TRI_AbortTransaction(this, 0);
  }

#ifdef ARANGODB_ENABLE_ROCKSDB
  delete _rocksTransaction;
#endif

  ReleaseCollections(this, 0);

  // free all collections
  for (auto it = _collections.rbegin(); it != _collections.rend(); ++it) {
    FreeCollection(*it);
  }
}

