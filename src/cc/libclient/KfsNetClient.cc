//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2009/05/20
// Author: Mike Ovsiannikov
//
// Copyright 2009-2012 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
//
//----------------------------------------------------------------------------

#include "KfsNetClient.h"

#include "kfsio/IOBuffer.h"
#include "kfsio/NetConnection.h"
#include "kfsio/NetManager.h"
#include "kfsio/ITimeout.h"
#include "kfsio/ClientAuthContext.h"
#include "kfsio/DelegationToken.h"
#include "kfsio/Resolver.h"
#include "common/kfstypes.h"
#include "common/kfsdecls.h"
#include "common/MsgLogger.h"
#include "common/StdAllocator.h"
#include "qcdio/QCUtils.h"
#include "qcdio/qcstutils.h"
#include "qcdio/QCDLList.h"
#include "qcdio/QCThread.h"
#include "KfsOps.h"
#include "utils.h"

#include <sstream>
#include <algorithm>
#include <map>
#include <deque>
#include <string>
#include <cerrno>
#include <iomanip>

#include <stdint.h>
#include <time.h>
#include <stdlib.h>

namespace KFS
{
namespace client
{
using std::string;
using std::map;
using std::list;
using std::pair;
using std::make_pair;
using std::less;
using std::max;
using std::hex;
using std::showbase;

const int64_t kMaxSessionTimeout             = int64_t(10) * 365 * 24 * 60 * 60;
const int64_t kSessionUpdateResolutionSec    = LEASE_INTERVAL_SECS / 2;
const int64_t kSessionChangeStartIntervalSec = 30 * 60;

class KfsClientRefCount
{
public:
    class StRef
    {
    public:
        StRef(
            KfsClientRefCount& inObj)
            : mObj(inObj)
            { mObj.Ref(1); }
        ~StRef()
            { mObj.UnRef(); }
    private:
        KfsClientRefCount& mObj;
    private:
        StRef(
            StRef& inRef);
        StRef& operator=(
            StRef& inRef);
    };

    KfsClientRefCount(
        const QCThread* inThreadPtr)
        : mRefCount(0),
          mThreadPtr(inThreadPtr)
        {}
    void Ref(
        int inMinRefCount = 0)
    {
        ++mRefCount;
        QCRTASSERT(
            (! mThreadPtr || mThreadPtr->IsCurrentThread()) &&
            inMinRefCount < mRefCount
        );
    }
    void UnRef()
    {
        QCRTASSERT(
            (! mThreadPtr || mThreadPtr->IsCurrentThread()) &&
            0 < mRefCount
        );
        if (--mRefCount == 0) {
            delete this;
        }
    }
    int GetRefCount() const
        { return mRefCount; }
    void SetThread(
        const QCThread* inThreadPtr)
        { mThreadPtr = inThreadPtr; }
protected:
    virtual ~KfsClientRefCount()
        { mRefCount = -23456; }
private:
    int             mRefCount;
    const QCThread* mThreadPtr;
private:
    KfsClientRefCount(
        const KfsClientRefCount& inObj);
    KfsClientRefCount& operator=(
        const KfsClientRefCount& inObj);
};

// Generic KFS request / response protocol state machine implementation.
class KfsNetClient::Impl :
    public KfsCallbackObj,
    public KfsClientRefCount,
    private ITimeout,
    private OpOwner
{
public:
    typedef KfsClientRefCount::StRef StRef;
    typedef KfsNetClient::RpcFormat  RpcFormat;

    Impl(
        string             inHost,
        int                inPort,
        int                inMaxRetryCount,
        int                inTimeSecBetweenRetries,
        int                inOpTimeoutSec,
        int                inIdleTimeoutSec,
        kfsSeq_t           inInitialSeqNum,
        const char*        inLogPrefixPtr,
        NetManager&        inNetManager,
        bool               inResetConnectionOnOpTimeoutFlag,
        int                inMaxContentLength,
        bool               inFailAllOpsOnOpTimeoutFlag,
        bool               inMaxOneOutstandingOpFlag,
        ClientAuthContext* inAuthContextPtr,
        const QCThread*    inThreadPtr)
        : KfsCallbackObj(),
          KfsClientRefCount(inThreadPtr),
          ITimeout(),
          OpOwner(),
          mServerLocation(inHost, inPort),
          mPendingOpQueue(),
          mQueueStack(),
          mConnPtr(),
          mNextSeqNum(max(kfsSeq_t(100), // allow to insert auth op(s) in front
            (inInitialSeqNum < 0 ? -inInitialSeqNum : inInitialSeqNum) >> 1)),
          mReadHeaderDoneFlag(false),
          mSleepingFlag(false),
          mDataReceivedFlag(false),
          mDataSentFlag(false),
          mAllDataSentFlag(false),
          mRetryConnectOnlyFlag(false),
          mIdleTimeoutFlag(false),
          mResetConnectionOnOpTimeoutFlag(inResetConnectionOnOpTimeoutFlag),
          mFailAllOpsOnOpTimeoutFlag(inFailAllOpsOnOpTimeoutFlag),
          mMaxOneOutstandingOpFlag(inMaxOneOutstandingOpFlag),
          mShutdownSslFlag(false),
          mSslShutdownInProgressFlag(false),
          mTimeSecBetweenRetries(inTimeSecBetweenRetries),
          mOpTimeoutSec(inOpTimeoutSec),
          mIdleTimeoutSec(inIdleTimeoutSec),
          mRetryCount(0),
          mNonAuthRetryCount(0),
          mContentLength(0),
          mMaxRetryCount(inMaxRetryCount),
          mMaxContentLength(inMaxContentLength),
          mAuthFailureCount(0),
          mMaxRpcHeaderLength(MAX_RPC_HEADER_LEN),
          mPendingBytesSend(0),
          mMetaLogWriteRetryCount(0),
          mMaxMetaLogWriteRetryCount(0),
          mRpcFormat(kRpcFormatUndef),
          mInFlightOpPtr(0),
          mOutstandingOpPtr(0),
          mInFlightRecvBufPtr(0),
          mCurOpIt(),
          mIstream(),
          mOstream(),
          mProperties(),
          mStats(),
          mDisconnectCount(0),
          mEventObserverPtr(0),
          mLogPrefix((inLogPrefixPtr && inLogPrefixPtr[0]) ?
                (inLogPrefixPtr + string(" ")) : string()),
          mNetManagerPtr(&inNetManager),
          mAuthContextPtr(inAuthContextPtr),
          mSessionExpirationTime(-1),
          mKeyExpirationTime(-1),
          mKeyId(),
          mKeyData(),
          mSessionKeyId(),
          mSessionKeyData(),
          mLookupOp(-1, ROOTFID, "/"),
          mAuthOp(-1, kAuthenticationTypeUndef),
          mMetaVrNodesCount(0),
          mMetaVrNodesActiveCount(0),
          mMetaLocations(),
          mMetaHostNames(),
          mResolverPtr(0)
    {
        SET_HANDLER(this, &KfsNetClient::Impl::EventHandler);
        MetaVrList::Init(mMetaVrListPtr);
        ResolverList::Init(mResolverReqsPtr);
    }
    bool IsConnected() const
        { return (mConnPtr && mConnPtr->IsGood()); }
    int64_t GetDisconnectCount() const
        { return mDisconnectCount; }
    bool Start(
        string             inServerName,
        int                inServerPort,
        string*            inErrMsgPtr,
        bool               inRetryPendingOpsFlag,
        int                inMaxRetryCount,
        int                inTimeSecBetweenRetries,
        bool               inRetryConnectOnlyFlag,
        ClientAuthContext* inAuthContextPtr)
    {
        if (! inRetryPendingOpsFlag) {
            Cancel();
        }
        mRetryConnectOnlyFlag  = inRetryConnectOnlyFlag;
        mMaxRetryCount         = inMaxRetryCount;
        mTimeSecBetweenRetries = inTimeSecBetweenRetries;
        mAuthContextPtr        = inAuthContextPtr;
        return SetServer(ServerLocation(inServerName, inServerPort),
            false, inErrMsgPtr, true);
    }
    bool SetServer(
        const ServerLocation& inLocation,
        bool                  inCancelPendingOpsFlag,
        string*               inErrMsgPtr,
        bool                  inForceConnectFlag)
    {
        if (inLocation == mServerLocation ||
                ! MetaVrList::IsEmpty(mMetaVrListPtr) ||
                ! mMetaLocations.empty()) {
            if (! inForceConnectFlag && mPendingOpQueue.empty()) {
                return inLocation.IsValid();
            }
        } else {
            if (inCancelPendingOpsFlag) {
                Cancel();
            }
            if (mSleepingFlag || IsConnected()) {
                Reset();
            }
            mServerLocation    = inLocation;
            mAuthFailureCount  = 0;
            mRetryCount        = 0;
            mNonAuthRetryCount = 0;
            mNextSeqNum += 100;
            if (! inForceConnectFlag && mPendingOpQueue.empty()) {
                return inLocation.IsValid();
            }
        }
        EnsureConnected(inErrMsgPtr);
        return (mSleepingFlag || IsConnected() ||
            0 < mMetaVrNodesActiveCount ||
            ! ResolverList::IsEmpty(mResolverReqsPtr)
        );
    }
    void SetRpcFormat(
        RpcFormat inRpcFormat)
        {  mRpcFormat = inRpcFormat; }
    RpcFormat GetRpcFormat() const
        { return mRpcFormat; }
    void SetAuthContext(
        ClientAuthContext* inAuthContextPtr)
        { mAuthContextPtr = inAuthContextPtr; }
    ClientAuthContext* GetAuthContext() const
        { return mAuthContextPtr; }
    void SetKey(
        const char* inKeyIdPtr,
        const char* inKeyDataPtr,
        int         inKeyDataSize)
        { SetKey(inKeyIdPtr, strlen(inKeyIdPtr), inKeyDataPtr, inKeyDataSize); }
    void SetKey(
        const char* inKeyIdPtr,
        int         inKeyIdLen,
        const char* inKeyDataPtr,
        int         inKeyDataSize)
    {
        if (inKeyIdPtr && inKeyIdLen > 0) {
            mKeyId.assign(inKeyIdPtr, (size_t)inKeyIdLen);
        } else {
            mKeyId.clear();
        }
        if (inKeyDataPtr && inKeyDataSize > 0) {
            mKeyData.assign(inKeyDataPtr, (size_t)inKeyDataSize);
            if (mKeyId.empty()) {
                mKeyExpirationTime = Now() + kMaxSessionTimeout;
            } else if (mKeyId != mSessionKeyId) {
                mKeyExpirationTime = GetTokenExpirationTime(mKeyId);
            }
        } else {
            mKeyData.clear();
        }
    }
    const string& GetKey() const
        { return mKeyData; }
    const string& GetKeyId() const
        { return mKeyId; }
    const string& GetSessionKey() const
        { return mSessionKeyData; }
    const string& GetSessionKeyId() const
        { return mSessionKeyId; }
    bool IsShutdownSsl() const
        { return mShutdownSslFlag; }
    void SetShutdownSsl(
        bool inFlag)
    {
        if (inFlag == mShutdownSslFlag) {
            return;
        }
        mShutdownSslFlag = inFlag;
        if (mShutdownSslFlag) {
            if (IsConnected() && mConnPtr->GetFilter()) {
                mSslShutdownInProgressFlag = true;
                const int theErr = mConnPtr->Shutdown();
                if (theErr != 0) {
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "ssl shutdown failure: " <<
                            theErr <<
                    KFS_LOG_EOM;
                    Reset();
                    if (! mPendingOpQueue.empty()) {
                        EnsureConnected();
                    }
                }
            }
        } else if (IsConnected()) {
            Reset();
            if (! mPendingOpQueue.empty()) {
                EnsureConnected();
            }
        }
    }
    void Reset()
    {
        if (mSleepingFlag) {
            mNetManagerPtr->UnRegisterTimeoutHandler(this);
            mSleepingFlag = false;
        }
        ResetConnection();
        CancelVrPrimaryCheck();
    }
    void CancelAllWithOwner(
        OpOwner* inOwnerPtr)
    {
        if (mInFlightOpPtr && mInFlightOpPtr->mOwnerPtr == inOwnerPtr) {
            CancelInFlightOp();
        }
        const bool thePendingEmptyFlag = mPendingOpQueue.empty();
        if (thePendingEmptyFlag && mQueueStack.empty()) {
            return;
        }
        if (! thePendingEmptyFlag) {
            OpQueue theQeue;
            for (OpQueue::iterator theIt = mPendingOpQueue.begin();
                    theIt != mPendingOpQueue.end();
                    ) {
                OpQueue::iterator const theCur = theIt++;
                if (theCur->second.mOwnerPtr == inOwnerPtr) {
                    theQeue.insert(*theCur);
                    mPendingOpQueue.erase(theCur);
                }
            }
            if (! theQeue.empty()) {
                QueueStack::iterator const theIt =
                    mQueueStack.insert(mQueueStack.end(), OpQueue());
                theQeue.swap(*theIt);
            }
        }
        for (QueueStack::iterator theStIt = mQueueStack.begin();
                theStIt != mQueueStack.end();
                ) {
            for (OpQueue::iterator theIt = theStIt->begin();
                    theIt != theStIt->end();
                    ++theIt) {
                if (theIt->second.mOwnerPtr != inOwnerPtr ||
                        ! theIt->second.mOpPtr) {
                    continue;
                }
                mStats.mOpsCancelledCount++;
                theIt->second.Cancel();
            }
            if (theStIt->empty()) {
                mQueueStack.erase(theStIt++);
            } else {
                ++theStIt;
            }
        }
    }
    void Stop()
    {
        Reset();
        mAuthFailureCount = 0;
        Cancel();
    }
    int GetMaxRetryCount() const
        { return mMaxRetryCount; }
    void SetMaxRetryCount(
        int inMaxRetryCount)
        { mMaxRetryCount = inMaxRetryCount; }
    int GetOpTimeoutSec() const
        { return mOpTimeoutSec; }
    void SetOpTimeoutSec(
        int inTimeout)
    {
        mOpTimeoutSec = inTimeout;
        if (IsConnected() && ! mPendingOpQueue.empty()) {
            mConnPtr->SetInactivityTimeout(mOpTimeoutSec);
        }
    }
    int GetIdleTimeoutSec() const
        { return mIdleTimeoutSec; }
    void SetIdleTimeoutSec(
        int inTimeout)
    {
        mIdleTimeoutSec = inTimeout;
        if (IsConnected() && mPendingOpQueue.empty()) {
            mConnPtr->SetInactivityTimeout(mIdleTimeoutSec);
        }
    }
    int GetTimeSecBetweenRetries() const
        { return mTimeSecBetweenRetries; }
    void SetTimeSecBetweenRetries(
        int inTimeSec)
        { mTimeSecBetweenRetries = inTimeSec; }
    bool IsAllDataSent() const
        { return (mDataSentFlag && mAllDataSentFlag); }
    bool IsDataReceived() const
        { return mDataReceivedFlag; }
    bool IsDataSent() const
        { return mDataSentFlag; }
    bool IsRetryConnectOnly() const
        { return mRetryConnectOnlyFlag; }
    bool WasDisconnected() const
        { return ((mDataSentFlag || mDataReceivedFlag) && ! IsConnected()); }
    void SetRetryConnectOnly(
        bool inFlag)
        { mRetryConnectOnlyFlag = inFlag; }
    void GetStats(
        Stats& outStats) const
        { outStats = mStats; }
    const ServerLocation& GetServerLocation() const
        { return mServerLocation; }
    bool Enqueue(
        KfsOp*    inOpPtr,
        OpOwner*  inOwnerPtr,
        IOBuffer* inBufferPtr = 0)
    {
        const time_t theNow = Now();
        if (mPendingOpQueue.empty() && IsConnected() &&
                (mSessionKeyId.empty() ?
                (mSessionExpirationTime < theNow + kSessionUpdateResolutionSec) :
                (((mSessionExpirationTime <
                        theNow + kSessionChangeStartIntervalSec &&
                    mSessionExpirationTime + kSessionUpdateResolutionSec <
                        mKeyExpirationTime) ||
                    mSessionExpirationTime < theNow) &&
                ! mKeyId.empty() && theNow < mKeyExpirationTime))) {
            KFS_LOG_STREAM_INFO << mLogPrefix <<
                "updating session by initiating re-connect" <<
                " expires: +" << (mSessionExpirationTime - theNow) <<
            KFS_LOG_EOM;
            ResetConnection();
        }
        // Ensure that the op is in the queue before attempting to re-establish
        // connection, as later can fail other ops, and invoke the op cancel.
        // The op has to be in the queue in order for cancel to work.
        mStats.mOpsQueuedCount++;
        const bool theOkFlag = EnqueueSelf(inOpPtr, inOwnerPtr, inBufferPtr, 0);
        if (theOkFlag) {
            EnsureConnected(0, inOpPtr);
        }
        return theOkFlag;
    }
    bool Cancel(
        KfsOp*   inOpPtr,
        OpOwner* inOwnerPtr)
    {
        if (! inOpPtr) {
            return true; // Nothing to do.
        }
        OpQueue::iterator theIt = mPendingOpQueue.find(inOpPtr->seq);
        if (theIt != mPendingOpQueue.end()) {
            if (theIt->second.mOwnerPtr != inOwnerPtr ||
                    theIt->second.mOpPtr != inOpPtr) {
                return false;
            }
            Cancel(theIt);
            return true;
        }
        for (QueueStack::iterator theStIt = mQueueStack.begin();
                theStIt != mQueueStack.end();
                ++theStIt) {
            if ((theIt = theStIt->find(inOpPtr->seq)) != theStIt->end()) {
                if (theIt->second.mOwnerPtr != inOwnerPtr ||
                        theIt->second.mOpPtr != inOpPtr) {
                    return false;
                }
                if (&theIt->second == mInFlightOpPtr) {
                    CancelInFlightOp();
                }
                theIt->second.Cancel();
                break;
            }
        }
        return true;
    }
    bool Cancel()
        { return CancelOrFailAll(0, string()); }
    bool Fail(
        int           inStatus,
        const string& inStatusMsg,
        bool          inDisconnectFlag = true)
    {
        if (inDisconnectFlag) {
            Reset();
        }
        return CancelOrFailAll(inStatus, inStatusMsg);
    }
    bool CancelOrFailAll(
        int           inStatus,
        const string& inStatusMsg)
    {
        CancelInFlightOp();
        const bool thePendingEmptyFlag = mPendingOpQueue.empty();
        if (thePendingEmptyFlag && mQueueStack.empty()) {
            return false;
        }
        QueueStack::iterator theIt;
        if (! thePendingEmptyFlag) {
            theIt = mQueueStack.insert(mQueueStack.end(), OpQueue());
            mPendingOpQueue.swap(*theIt);
        }
        for (QueueStack::iterator theStIt = mQueueStack.begin();
                theStIt != mQueueStack.end();
                ++theStIt) {
            for (OpQueue::iterator theIt = theStIt->begin();
                    theIt != theStIt->end();
                    ++theIt) {
                if (! theIt->second.mOpPtr) {
                    continue;
                }
                mStats.mOpsCancelledCount++;
                if (inStatus == 0) {
                    theIt->second.Cancel();
                } else {
                    theIt->second.mOpPtr->status    = inStatus;
                    theIt->second.mOpPtr->statusMsg = inStatusMsg;
                    theIt->second.Done();
                }
            }
        }
        if (! thePendingEmptyFlag) {
            mQueueStack.erase(theIt);
        }
        return true;
    }
    int EventHandler(
        int   inCode,
        void* inDataPtr)
    {
        const int thePrevRefCount = GetRefCount();
        StRef theRef(*this);

        if (mEventObserverPtr && mEventObserverPtr->Event(inCode, inDataPtr)) {
            return 0;
        }

        const char*         theReasonPtr        = "network error";
        OpQueueEntry* const theOutstandingOpPtr = mOutstandingOpPtr;
        int                 theError            = 0;
        switch (inCode) {
            case EVENT_NET_READ: {
                    assert(inDataPtr && mConnPtr &&
                        &mConnPtr->GetInBuffer() == inDataPtr);
                    IOBuffer& theBuffer = mConnPtr->GetInBuffer();
                    mDataReceivedFlag = mDataReceivedFlag ||
                        (! theBuffer.IsEmpty() && ! IsAuthInFlight());
                    HandleResponse(theBuffer);
                }
                break;

            case EVENT_NET_WROTE:
                if (mConnPtr) {
                    const int theRem = mConnPtr->GetOutBuffer().BytesConsumable();
                    if (theRem < mPendingBytesSend) {
                        mStats.mBytesSentCount += mPendingBytesSend - theRem;
                    }
                    mPendingBytesSend = theRem;
                }
                assert(inDataPtr && mConnPtr);
                mDataSentFlag = mDataSentFlag || ! IsAuthInFlight();
                break;

            case EVENT_INACTIVITY_TIMEOUT:
                if (! mIdleTimeoutFlag &&
                        IsConnected() && mPendingOpQueue.empty()) {
                    mConnPtr->SetInactivityTimeout(mIdleTimeoutSec);
                    mIdleTimeoutFlag = true;
                    break;
                }
                theReasonPtr = "inactivity timeout";
                theError     = -ETIMEDOUT;
                // Fall through.
            case EVENT_NET_ERROR:
                if (mConnPtr) {
                    if (mSslShutdownInProgressFlag && mConnPtr->IsGood()) {
                        KFS_LOG_STREAM(mConnPtr->GetFilter() ?
                                MsgLogger::kLogLevelERROR :
                                MsgLogger::kLogLevelDEBUG) << mLogPrefix <<
                            "ssl shutdown completion:"
                            " filter: " << reinterpret_cast<const void*>(
                                mConnPtr->GetFilter()) <<
                        KFS_LOG_EOM;
                        mSslShutdownInProgressFlag = false;
                        if (! mConnPtr->GetFilter()) {
                            mConnPtr->StartFlush();
                            break;
                        }
                    }
                    if (mAuthContextPtr && mConnPtr->IsAuthFailure()) {
                        mAuthFailureCount++;
                        theError = -EPERM;
                    } else {
                        mAuthFailureCount = 0;
                        if (0 == theError) {
                            if (inDataPtr) {
                                theError =
                                    *reinterpret_cast<const int*>(inDataPtr);
                                if (0 <= theError) {
                                    theError = -EIO;
                                }
                            } else {
                                theError = -EIO;
                            }
                        }
                    }
                    mAllDataSentFlag = ! mConnPtr->IsWriteReady();
                    KFS_LOG_STREAM(mPendingOpQueue.empty() ?
                            MsgLogger::kLogLevelDEBUG :
                            MsgLogger::kLogLevelERROR) << mLogPrefix <<
                        "closing connection: " << mConnPtr->GetSockName() <<
                        " to: "            << mServerLocation <<
                        " due to "         << theReasonPtr <<
                        " pending:"
                        " read: "          << mConnPtr->GetNumBytesToRead() <<
                        " write: "         << mConnPtr->GetNumBytesToWrite() <<
                        " ops: "           << mPendingOpQueue.size() <<
                        " auth failures: " << mAuthFailureCount <<
                        " error: "         << mConnPtr->GetErrorMsg() <<
                    KFS_LOG_EOM;
                    Reset();
                }
                if (mIdleTimeoutFlag) {
                    mStats.mConnectionIdleTimeoutCount++;
                } else if (mPendingOpQueue.empty()) {
                    break;
                } else if (mDataSentFlag || mDataReceivedFlag) {
                    mStats.mNetErrorCount++;
                    if (inCode == EVENT_INACTIVITY_TIMEOUT) {
                        mStats.mResponseTimeoutCount++;
                    }
                } else {
                    mStats.mConnectFailureCount++;
                }
                if (! mPendingOpQueue.empty()) {
                    RetryConnect(theOutstandingOpPtr, theError);
                }
                break;

            default:
                assert(!"Unknown event");
                break;
        }
        if (thePrevRefCount <= GetRefCount()) {
            OpsTimeout();
        }
        return 0;
    }
    void SetEventObserver(
        EventObserver* inEventObserverPtr)
        { mEventObserverPtr = inEventObserverPtr; }
    time_t Now() const
        { return mNetManagerPtr->Now(); }
    NetManager& GetNetManager() const
        { return *mNetManagerPtr; }
    void SetMaxContentLength(
        int inMax)
        { mMaxContentLength = inMax; }
    void SetMaxRpcHeaderLength(
        int inMaxLength)
        { mMaxRpcHeaderLength = inMaxLength; }
    void ClearMaxOneOutstandingOpFlag()
    {
        if (! mMaxOneOutstandingOpFlag) {
            return;
        }
        mMaxOneOutstandingOpFlag = false;
        OpQueueEntry* const theOutstandingOpPtr = mOutstandingOpPtr;
        mOutstandingOpPtr = 0;
        bool theResetTimerFlag = ! mOutstandingOpPtr;
        for (OpQueue::iterator theIt = mPendingOpQueue.begin();
                theIt != mPendingOpQueue.end() && IsConnected();
                ++theIt) {
            OpQueueEntry& theEntry = theIt->second;
            if (&theEntry != theOutstandingOpPtr) {
                Request(theEntry, theResetTimerFlag, theEntry.mRetryCount);
                theResetTimerFlag = false;
            }
        }
    }
    void SetFailAllOpsOnOpTimeoutFlag(
        bool inFlag)
    {
        mFailAllOpsOnOpTimeoutFlag = inFlag;
    }
    virtual void OpDone(
        KfsOp*    inOpPtr,
        bool      inCanceledFlag,
        IOBuffer* inBufferPtr)
    {
        assert(! inBufferPtr && ! mOutstandingOpPtr &&
            (inOpPtr == &mLookupOp || inOpPtr == &mAuthOp));
        KFS_LOG_STREAM_DEBUG << mLogPrefix << mServerLocation <<
            (inCanceledFlag ? " op canceled: " : " op done: ") <<
            inOpPtr->Show() <<
            " now: "    << Now() <<
            " status: " << inOpPtr->status <<
            " msg: "    << inOpPtr->statusMsg <<
        KFS_LOG_EOM;
        const kfsSeq_t theSeq = inOpPtr->seq;
        inOpPtr->seq = -1;
        if (inCanceledFlag) {
            return;
        }
        if (inOpPtr->status == 0 && ! IsConnected()) {
            if (! mPendingOpQueue.empty()) {
                EnsureConnected();
            }
            return;
        }
        if (inOpPtr == &mLookupOp) {
            if ((mLookupOp.status == 0 || mLookupOp.status == -EACCES) &&
                    mLookupOp.authType == kAuthenticationTypeUndef) {
                // Does not support or understand authentication.
                if (! IsAuthEnabled()) {
                    return;
                }
                // Reset the status -- use auth type.
                mLookupOp.status = 0;
            }
            bool theDoAuthFlag = true;
            if (mLookupOp.status != 0 ||
                    (mAuthContextPtr && (mLookupOp.status =
                        mAuthContextPtr->CheckAuthType(
                            mLookupOp.authType,
                            theDoAuthFlag,
                            &mLookupOp.statusMsg)) != 0)) {
                KFS_LOG_STREAM_ERROR << mLogPrefix <<
                    "authentication negotiation failure: " <<
                        mLookupOp.statusMsg <<
                KFS_LOG_EOM;
                Fail(mLookupOp.status, mLookupOp.statusMsg);
                return;
            }
            if (! theDoAuthFlag) {
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "no auth. supported and/or required"
                    " auth. type: " << showbase << hex << mLookupOp.authType <<
                KFS_LOG_EOM;
                SubmitPending();
                return;
            }
            if (mAuthContextPtr) {
                assert(mAuthOp.seq < 0);
                const char* theBufPtr = 0;
                int         theBufLen = 0;
                mAuthOp.statusMsg.clear();
                if ((mAuthOp.status = mAuthContextPtr->Request(
                        mLookupOp.authType,
                        mAuthOp.requestedAuthType,
                        theBufPtr,
                        theBufLen,
                        mAuthRequestCtx,
                        &mAuthOp.statusMsg)) != 0) {
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "authentication request failure: " <<
                            mAuthOp.status <<
                        " " << mAuthOp.statusMsg <<
                    KFS_LOG_EOM;
                    Fail(mAuthOp.status, mAuthOp.statusMsg);
                    return;
                }
                const bool kOwnsContentBufFlag = false;
                mAuthOp.AttachContentBuf(
                    theBufPtr, theBufLen, kOwnsContentBufFlag);
                mAuthOp.contentLength = theBufLen;
                mAuthOp.seq           = theSeq + 1;
                EnqueueAuth(mAuthOp);
                return;
            }
        }
        if (inOpPtr != &mAuthOp) {
            KFS_LOG_STREAM_FATAL << "invalid op completion: " <<
                inOpPtr->Show() <<
            KFS_LOG_EOM;
            MsgLogger::Stop();
            abort();
            return;
        }
        if (! IsAuthEnabled() || ! mConnPtr) {
            Reset();
            if (! mPendingOpQueue.empty()) {
                EnsureConnected();
            }
            return;
        }
        if (mAuthOp.status == 0) {
            if ((mAuthOp.status = mAuthContextPtr->Response(
                    mAuthOp.chosenAuthType,
                    mAuthOp.useSslFlag,
                    mAuthOp.contentBuf,
                    mAuthOp.contentLength,
                    *mConnPtr,
                    mAuthRequestCtx,
                    &mAuthOp.statusMsg)) == 0 &&
                    IsConnected()) {
                const int kMaxAuthResponseTimeSec = 4;
                bool      theUseEndTimeFlag       = false;
                int64_t   theEndTime              = 0;
                if (mAuthOp.chosenAuthType == kAuthenticationTypePSK) {
                    theUseEndTimeFlag = ParseTokenExpirationTime(
                            mAuthContextPtr->GetPskId(), theEndTime);
                } else if (mAuthOp.chosenAuthType == kAuthenticationTypeX509) {
                    theUseEndTimeFlag =
                        mAuthContextPtr->GetX509EndTime(theEndTime);
                }
                if (mAuthOp.currentTime < mAuthOp.sessionEndTime) {
                    mSessionExpirationTime = Now() - kMaxAuthResponseTimeSec -
                        mAuthOp.currentTime + mAuthOp.sessionEndTime;
                    if (theUseEndTimeFlag) {
                        mSessionExpirationTime =
                            min(mSessionExpirationTime, theEndTime);
                    }
                } else if (theUseEndTimeFlag) {
                    mSessionExpirationTime = theEndTime;
                }
                mSessionExpirationTime = max(
                    mSessionExpirationTime,
                    Now() + kSessionUpdateResolutionSec + 4
                );
                mRetryCount = mNonAuthRetryCount;
                mNonAuthRetryCount = 0;
                SubmitPending();
                return;
            }
            if (mAuthOp.status == -EAGAIN) {
                EnsureConnected(); // Retry authentication.
                return;
            }
        }
        KFS_LOG_STREAM_ERROR << mLogPrefix <<
            "authentication response failure:"
            " seq: " << theSeq <<
            " "      << mAuthOp.status <<
            " "      << mAuthOp.statusMsg <<
            " "      << mAuthOp.Show() <<
        KFS_LOG_EOM;
        Fail(mAuthOp.status, mAuthOp.statusMsg);
    }
    void SetCommonRpcHeaders(
        const string& inCommonHeaders,
        const string& inCommonShortHeaders)
    {
        mCommonHeaders      = inCommonHeaders;
        mCommonShortHeaders = inCommonShortHeaders;
    }
    void SetNetManager(
        NetManager& inNetManager)
    {
        if (&inNetManager == mNetManagerPtr) {
            return;
        }
        Stop();
        mNetManagerPtr = &inNetManager;
        if (mResolverPtr) {
            mResolverPtr->Shutdown();
            delete mResolverPtr;
            mResolverPtr = 0;
        }
    }
    int GetMaxMetaLogWriteRetryCount() const
        { return mMaxMetaLogWriteRetryCount; }
    void SetMaxMetaLogWriteRetryCount(
        int inCount)
        { mMaxMetaLogWriteRetryCount = inCount; }
    bool AddMetaServerLocation(
        const ServerLocation& inLocation,
        bool                  inAllowDuplicatesFlag)
    {
        if (! inLocation.IsValid()) {
            return false;
        }
        if (TcpSocket::IsValidConnectToIpAddress(inLocation.hostname.c_str())) {
            if (! inAllowDuplicatesFlag) {
                MetaVrList::Iterator             theIt(mMetaVrListPtr);
                const MetaCheckVrPrimaryChecker* thePtr;
                while ((thePtr = theIt.Next())) {
                    if (thePtr->GetLocation() == inLocation) {
                        return false;
                    }
                }
            }
            const bool kLocationResolvedFlag = false;
            MetaVrList::PushBack(mMetaVrListPtr,
                *(new MetaCheckVrPrimaryChecker(
                    inLocation, *this, kLocationResolvedFlag)));
            mMetaVrNodesCount++;
            return true;
        }
        if (! inAllowDuplicatesFlag &&
                find(mMetaLocations.begin(), mMetaLocations.end(), inLocation) !=
                    mMetaLocations.end()) {
            return false;
        }
        mMetaLocations.push_back(inLocation);
        if (find(mMetaHostNames.begin(), mMetaHostNames.end(),
                inLocation.hostname) == mMetaHostNames.end()) {
            mMetaHostNames.push_back(inLocation.hostname);
        }
        return true;
    }
    int SetMetaServerLocations(
        const ServerLocation& inLocation,
        const char*           inLocationsStrPtr,
        size_t                inLocationsStrLen,
        bool                  inAllowDuplicatesFlag,
        bool                  inHexFormatFlag)
    {
        ClearMetaServerLocations();
        const char*       thePtr    = inLocationsStrPtr;
        const char* const theEndPtr = thePtr + inLocationsStrLen;
        int               theCount  = 0;
        ServerLocation    theLocation;
        while (thePtr < theEndPtr) {
            theLocation.Reset(0, -1);
            if (! theLocation.ParseString(
                    thePtr, theEndPtr - thePtr, inHexFormatFlag)) {
                KFS_LOG_STREAM_ERROR <<
                    "meta server node address parse failure: " << thePtr <<
                KFS_LOG_EOM;
                break;
            }
            if (AddMetaServerLocation(theLocation, inAllowDuplicatesFlag)) {
                theCount++;
            } else {
                KFS_LOG_STREAM_ERROR <<
                    "ignoring invalid meta server node address: " <<
                        theLocation <<
                KFS_LOG_EOM;
            }
            while (thePtr < theEndPtr && (*thePtr & 0xFF) <= ' ') {
                thePtr++;
            }
        }
        if (! inLocation.hostname.empty() && 0 < inLocation.port) {
            const bool kAllowDuplicatesFlag = false;
            if (AddMetaServerLocation(inLocation, kAllowDuplicatesFlag)) {
                theCount++;
            }
        }
        return theCount;
    }
    void ClearMetaServerLocations()
    {
        CancelVrPrimaryCheck();
        MetaCheckVrPrimaryChecker* thePtr;
        while ((thePtr = MetaVrList::PopFront(mMetaVrListPtr))) {
            delete thePtr;
        }
        mMetaLocations.clear();
    }
private:
    class DoNotDeallocate
    {
    public:
        DoNotDeallocate()
            {}
        void operator()(
            char* /* inBufferPtr */)
            {}
    };
    struct OpQueueEntry
    {
        OpQueueEntry(
            KfsOp*    inOpPtr     = 0,
            OpOwner*  inOwnerPtr  = 0,
            IOBuffer* inBufferPtr = 0)
            : mOpPtr(inOpPtr),
              mOwnerPtr(inOwnerPtr),
              mBufferPtr(inBufferPtr),
              mTime(0),
              mRetryCount(0),
              mVrConnectPendingFlag(false)
            {}
        void Cancel()
            { OpDone(true); }
        void Done()
            { OpDone(false); }
        void OpDone(
            bool inCanceledFlag)
        {
            KfsOp*    const theOpPtr     = mOpPtr;
            OpOwner*  const theOwnerPtr  = mOwnerPtr;
            IOBuffer* const theBufferPtr = mBufferPtr;
            Clear();
            if (theOwnerPtr) {
                if (theOpPtr) {
                    theOwnerPtr->OpDone(theOpPtr, inCanceledFlag, theBufferPtr);
                }
            } else {
                delete theOpPtr;
                delete theBufferPtr;
            }
        }
        void Clear()
        {
            mOpPtr     = 0;
            mOwnerPtr  = 0;
            mBufferPtr = 0;
        }
        KfsOp*    mOpPtr;
        OpOwner*  mOwnerPtr;
        IOBuffer* mBufferPtr;
        time_t    mTime;
        int       mRetryCount;
        bool      mVrConnectPendingFlag;
    };
    typedef map<kfsSeq_t, OpQueueEntry, less<kfsSeq_t>,
        StdFastAllocator<pair<const kfsSeq_t, OpQueueEntry> >
    > OpQueue;
    typedef list<OpQueue,
        StdFastAllocator<OpQueue>
    > QueueStack;
    enum { kMaxReadAhead = 4 << 10 };
    typedef ClientAuthContext::RequestCtx AuthRequestCtx;

    class MetaCheckVrPrimaryChecker : public KfsCallbackObj, public ITimeout
    {
    public:
        typedef QCDLList<MetaCheckVrPrimaryChecker> List;

        MetaCheckVrPrimaryChecker(
            const ServerLocation& inLocation,
            Impl&                 inOuter,
            bool                  inResolvedFlag)
            : KfsCallbackObj(),
              ITimeout(),
              mOuter(inOuter),
              mLocation(inLocation),
              mConnPtr(),
              mSeq(-1),
              mPendingBytesSend(0),
              mRetryCount(0),
              mSleepingFlag(false),
              mResolvedFlag(inResolvedFlag)
        {
            SET_HANDLER(this, &MetaCheckVrPrimaryChecker::EventHandler);
            List::Init(*this);
        }
        virtual ~MetaCheckVrPrimaryChecker()
            { MetaCheckVrPrimaryChecker::Reset(); }
        void Connect()
        {
            Reset();
            mOuter.mLookupOp.status = mOuter.Connect(
                mLocation, mConnPtr, *this, &mOuter.mLookupOp.statusMsg);
            if (0 == mOuter.mLookupOp.status) {
                Request();
            } else {
                Retry();
            }
        }
        int EventHandler(
            int   inCode,
            void* inDataPtr)
        {
            if (mOuter.mEventObserverPtr &&
                    mOuter.mEventObserverPtr->Event(inCode, inDataPtr)) {
                return 0;
            }
            const char* theReasonPtr = "network error";
            switch (inCode) {
                case EVENT_NET_READ: {
                        assert(inDataPtr && mConnPtr &&
                            &mConnPtr->GetInBuffer() == inDataPtr);
                        HandleResponse(mConnPtr->GetInBuffer());
                    }
                    break;

                case EVENT_NET_WROTE:
                    if (mConnPtr) {
                        const int theRem =
                            mConnPtr->GetOutBuffer().BytesConsumable();
                        if (theRem < mPendingBytesSend) {
                            mOuter.mStats.mBytesSentCount +=
                                mPendingBytesSend - theRem;
                        }
                        mPendingBytesSend = theRem;
                    }
                    break;

                case EVENT_INACTIVITY_TIMEOUT:
                    theReasonPtr = "request timed out";
                    mOuter.mStats.mResponseTimeoutCount++;
                    // Fall through.
                case EVENT_NET_ERROR:
                    mOuter.mStats.mNetErrorCount++;
                    KFS_LOG_STREAM_DEBUG << mOuter.mLogPrefix <<
                        "VR checker: "  << mLocation <<
                        " closing"
                        " connection: " << mConnPtr->GetSockName() <<
                        " due to "      << theReasonPtr <<
                        " error: "      << mConnPtr->GetErrorMsg() <<
                        " pending:"
                        " read: "       << mConnPtr->GetNumBytesToRead() <<
                        " write: "      << mConnPtr->GetNumBytesToWrite() <<
                    KFS_LOG_EOM;
                    if (EVENT_INACTIVITY_TIMEOUT == inCode) {
                        mOuter.mLookupOp.status = -ETIMEDOUT;
                    } else {
                        mOuter.mLookupOp.status = mConnPtr->GetErrorCode();
                        if (0 <= mOuter.mLookupOp.status) {
                            mOuter.mLookupOp.status = -EIO;
                        }
                    }
                    mOuter.mLookupOp.statusMsg = theReasonPtr;
                    mOuter.mLookupOp.statusMsg += ", ";
                    mOuter.mLookupOp.statusMsg += mConnPtr->GetErrorMsg();
                    Retry();
                    break;

                default:
                    assert(!"unexpected event");
                    break;
            }
            return 0;
        }
        void Reset()
        {
            if (mSleepingFlag) {
                mOuter.mNetManagerPtr->UnRegisterTimeoutHandler(this);
                mSleepingFlag = false;
            }
            if (mConnPtr) {
                mConnPtr->Close();
                mConnPtr.reset();
            }
            mPendingBytesSend = 0;
        }
        bool IsActive() const
            { return (0 != mConnPtr || mSleepingFlag); }
        const ServerLocation& GetLocation() const
            { return mLocation; }
        bool IsLocationResolved() const
            { return mResolvedFlag; }
        virtual void Timeout()
        {
            if (mSleepingFlag) {
                mOuter.mNetManagerPtr->UnRegisterTimeoutHandler(this);
                mSleepingFlag = false;
            }
            Connect();
        }
    private:
        Impl&                      mOuter;
        ServerLocation const       mLocation;
        NetConnectionPtr           mConnPtr;
        kfsSeq_t                   mSeq;
        int                        mPendingBytesSend;
        int                        mRetryCount;
        bool                       mSleepingFlag;
        bool const                 mResolvedFlag;
        MetaCheckVrPrimaryChecker* mPrevPtr[1];
        MetaCheckVrPrimaryChecker* mNextPtr[1];

        friend class QCDLListOp<MetaCheckVrPrimaryChecker>;

        void Request()
        {
            mOuter.InitHelloLookupOp();
            ReqOstream theStream(mOuter.mOstream.Set(mConnPtr->GetOutBuffer()));
            mOuter.mLookupOp.Request(theStream);
            mOuter.mOstream.Reset();
            mSeq = mOuter.mLookupOp.seq;
            mOuter.mLookupOp.seq = -1;
            mPendingBytesSend = mConnPtr->GetOutBuffer().BytesConsumable();
            mConnPtr->StartFlush();
        }
        void HandleResponse(
            IOBuffer& inBuffer)
        {
            kfsSeq_t  theOpSeq         = -1;
            bool      theErrorFlag     = false;
            int       theContentLength = -1;
            RpcFormat theRpcFormat     = kRpcFormatUndef;
            if (mOuter.ReadHeaderSelf(
                        inBuffer,
                        mOuter.mProperties,
                        theOpSeq,
                        theRpcFormat,
                        theContentLength,
                        theErrorFlag)) {
                if (0 < theContentLength) {
                    mOuter.mLookupOp.status    = -EINVAL;
                    mOuter.mLookupOp.statusMsg =
                        "invalid non zero content length filed";
                } else if (mSeq != theOpSeq) {
                    mOuter.mLookupOp.status    = -EINVAL;
                    mOuter.mLookupOp.statusMsg = "op sequence mismatch";
                } else {
                    mOuter.mLookupOp.shortRpcFormatFlag =
                        kRpcFormatShort == theRpcFormat;
                    mOuter.mLookupOp.status = 0;
                    mOuter.mLookupOp.statusMsg.clear();
                    mOuter.mLookupOp.ParseResponseHeader(mOuter.mProperties);
                }
            } else {
                if (! theErrorFlag) {
                    return;
                }
                mOuter.mLookupOp.status    = -EINVAL;
                mOuter.mLookupOp.statusMsg = "response parse error";
            }
            mOuter.mProperties.clear();
            if ((0 == mOuter.mLookupOp.status ||
                    (-EACCES == mOuter.mLookupOp.status &&
                        ! mOuter.mLookupOp.responseHasVrPrimaryKeyFlag)) &&
                    (mOuter.mLookupOp.vrPrimaryFlag ||
                        ! mOuter.mLookupOp.responseHasVrPrimaryKeyFlag)) {
                NetConnectionPtr theConnPtr;
                theConnPtr.swap(mConnPtr);
                mOuter.mLookupOp.seq = mSeq;
                Reset();
                mOuter.SetVrPrimary(theConnPtr, mLocation, theRpcFormat);
                return;
            }
            Reset();
            Retry(0 == mOuter.mLookupOp.status);
        }
        void Retry(
            bool inNotPrimaryFlag = false)
        {
            Reset();
            const int theMaxRetryCnt = inNotPrimaryFlag ?
                max(mOuter.mMaxMetaLogWriteRetryCount, mOuter.mMaxRetryCount) :
                mOuter.mMaxRetryCount;
            if (theMaxRetryCnt < ++mRetryCount) {
                if (0 <= mOuter.mLookupOp.status) {
                    mOuter.mLookupOp.status    = -EIO;
                    mOuter.mLookupOp.statusMsg = "retry limit reached";
                }
                mOuter.mLookupOp.seq = -1;
                mOuter.SetVrPrimary(mConnPtr, mLocation, kRpcFormatUndef);
                return;
            }
            KFS_LOG_STREAM_DEBUG << mOuter.mLogPrefix <<
                "VR checker: "        << mLocation <<
                " retry attempt "     << mRetryCount <<
                " of "                << mOuter.mMaxRetryCount <<
                ", scheduling retry " << mOuter.mPendingOpQueue.size() <<
                " pending operation(s)"
                " in "                << mOuter.mTimeSecBetweenRetries <<
                " seconds" <<
            KFS_LOG_EOM;
            if (0 < mOuter.mTimeSecBetweenRetries ) {
                mOuter.mStats.mSleepTimeSec += mOuter.mTimeSecBetweenRetries;
                SetTimeoutInterval(
                    mOuter.mTimeSecBetweenRetries * 1000, true);
                mSleepingFlag = true;
                mOuter.mNetManagerPtr->RegisterTimeoutHandler(this);
            } else {
                Timeout();
            }
        }
    private:
        MetaCheckVrPrimaryChecker(
            const MetaCheckVrPrimaryChecker& inChecker);
        MetaCheckVrPrimaryChecker& operator=(
            const MetaCheckVrPrimaryChecker& inChecker);
    };
    friend class MetaCheckVrPrimaryChecker;
    typedef MetaCheckVrPrimaryChecker::List MetaVrList;

    class ResolverReq : public Resolver::Request, public ITimeout
    {
    public:
        typedef QCDLList<ResolverReq>          List;
        typedef Resolver::Request::IpAddresses IpAddresses;

        ResolverReq(
            const string& inHostName,
            Impl&         inImpl)
            : Resolver::Request(inHostName),
              ITimeout(),
              mImplPtr(&inImpl),
              mRetryCount(0)
            { List::Init(*this); }
        const string& GetHostName() const
            { return mHostName; }
        virtual void Done()
        {
            if (! mImplPtr) {
                delete this;
                return;
            }
            if (0 != mStatus && ++mRetryCount <= mImplPtr->mMaxRetryCount) {
                KFS_LOG_STREAM_ERROR << mImplPtr->mLogPrefix <<
                    "resolver: "          << mHostName <<
                    " status: "           << mStatus <<
                    " "                   << mStatusMsg <<
                    " retry attempt "     << mRetryCount <<
                    " of "                << mImplPtr->mMaxRetryCount <<
                    ", scheduling retry " << mImplPtr->mPendingOpQueue.size() <<
                    " pending operation(s)"
                    " in "                << mImplPtr->mTimeSecBetweenRetries <<
                    " seconds" <<
                KFS_LOG_EOM;
                if (0 < mImplPtr->mTimeSecBetweenRetries) {
                    SetTimeoutInterval(
                        mImplPtr->mTimeSecBetweenRetries * 1000, true);
                    mSleepingFlag = true;
                    mImplPtr->mNetManagerPtr->RegisterTimeoutHandler(this);
                    return;
                }
                Timeout();
                return;
            }
            mImplPtr->Resolved(*this);
        }
        virtual void Timeout()
        {
            if (mSleepingFlag) {
                mImplPtr->mNetManagerPtr->UnRegisterTimeoutHandler(this);
                mSleepingFlag = false;
            }
            if (mImplPtr) {
                mImplPtr->mResolverPtr->Enqueue(*this);
            }
        }
        const IpAddresses& GetIps() const
            { return mIpAddresses; }
        int GetStatus() const
            { return mStatus; }
        const string& GetStatusMsg() const
            { return mStatusMsg; }
        void Delete(
            ResolverReq** inListPtr)
        {
            if (mSleepingFlag) {
                mImplPtr->mNetManagerPtr->UnRegisterTimeoutHandler(this);
                mSleepingFlag = false;
            }
            const bool theInFlightFlag = List::IsInList(inListPtr, *this);
            List::Remove(inListPtr, *this);
            if (theInFlightFlag) {
                mImplPtr = 0;
                return;
            }
            delete this;
        }
    private:
        Impl*        mImplPtr;
        int          mRetryCount;
        bool         mSleepingFlag;
        ResolverReq* mPrevPtr[1];
        ResolverReq* mNextPtr[1];

        friend class QCDLListOp<ResolverReq>;

        virtual ~ResolverReq()
            {}
    private:
        ResolverReq(
            const ResolverReq& inReq);
        ResolverReq& operator=(
            const ResolverReq& inReq);
    };
    friend class ResolverReq;
    typedef ResolverReq::List ResolverList;
    typedef vector<string> MetaHostNames;
    typedef vector<ServerLocation> MetaLocations;

    ServerLocation             mServerLocation;
    OpQueue                    mPendingOpQueue;
    QueueStack                 mQueueStack;
    NetConnectionPtr           mConnPtr;
    kfsSeq_t                   mNextSeqNum;
    bool                       mReadHeaderDoneFlag;
    bool                       mSleepingFlag;
    bool                       mDataReceivedFlag;
    bool                       mDataSentFlag;
    bool                       mAllDataSentFlag;
    bool                       mRetryConnectOnlyFlag;
    bool                       mIdleTimeoutFlag;
    bool                       mPrevAuthFailureFlag;
    bool                       mResetConnectionOnOpTimeoutFlag;
    bool                       mFailAllOpsOnOpTimeoutFlag;
    bool                       mMaxOneOutstandingOpFlag;
    bool                       mShutdownSslFlag;
    bool                       mSslShutdownInProgressFlag;
    int                        mTimeSecBetweenRetries;
    int                        mOpTimeoutSec;
    int                        mIdleTimeoutSec;
    int                        mRetryCount;
    int                        mNonAuthRetryCount;
    int                        mContentLength;
    int                        mMaxRetryCount;
    int                        mMaxContentLength;
    int                        mAuthFailureCount;
    int                        mMaxRpcHeaderLength;
    int                        mPendingBytesSend;
    int                        mMetaLogWriteRetryCount;
    int                        mMaxMetaLogWriteRetryCount;
    RpcFormat                  mRpcFormat;
    OpQueueEntry*              mInFlightOpPtr;
    OpQueueEntry*              mOutstandingOpPtr;
    char*                      mInFlightRecvBufPtr;
    OpQueue::iterator          mCurOpIt;
    IOBuffer::IStream          mIstream;
    IOBuffer::WOStream         mOstream;
    Properties                 mProperties;
    Stats                      mStats;
    int64_t                    mDisconnectCount;
    EventObserver*             mEventObserverPtr;
    const string               mLogPrefix;
    NetManager*                mNetManagerPtr;
    ClientAuthContext*         mAuthContextPtr;
    int64_t                    mSessionExpirationTime;
    int64_t                    mKeyExpirationTime;
    string                     mKeyId;
    string                     mKeyData;
    string                     mSessionKeyId;
    string                     mSessionKeyData;
    string                     mCommonHeaders;
    string                     mCommonShortHeaders;
    AuthRequestCtx             mAuthRequestCtx;
    LookupOp                   mLookupOp;
    AuthenticateOp             mAuthOp;
    int                        mMetaVrNodesCount;
    int                        mMetaVrNodesActiveCount;
    MetaLocations              mMetaLocations;
    MetaHostNames              mMetaHostNames;
    Resolver*                  mResolverPtr;
    ResolverReq*               mResolverReqsPtr[1];
    MetaCheckVrPrimaryChecker* mMetaVrListPtr[1];

    virtual ~Impl()
    {
        Impl::Reset();
        ClearMetaServerLocations();
        if (mResolverPtr) {
            mResolverPtr->Shutdown();
            delete mResolverPtr;
        }
    }
    bool IsAuthInFlight()
        { return (0 <= mLookupOp.seq || 0 <= mAuthOp.seq); }
    void SetMaxWaitTime(
        KfsOp& inOp)
    {
        inOp.maxWaitMillisec = mOpTimeoutSec > 0 ?
            int64_t(mOpTimeoutSec) * 1000 : int64_t(-1);
    }
    void InitHelloLookupOp()
    {
        mLookupOp.DeallocContentBuf();
        mLookupOp.contentLength               = 0;
        mLookupOp.status                      = 0;
        mLookupOp.statusMsg.clear();
        mLookupOp.reqShortRpcFormatFlag       = mRpcFormat == kRpcFormatUndef;
        mLookupOp.authType                    = kAuthenticationTypeNone;
        mLookupOp.vrPrimaryFlag               = false;
        mLookupOp.responseHasVrPrimaryKeyFlag = false;
        mLookupOp.seq                         = mNextSeqNum++;
    }
    void SetVrPrimary(
        NetConnectionPtr&     inConnPtr,
        const ServerLocation& inLocation,
        RpcFormat             inRpcFormat)
    {
        if (inConnPtr) {
            const kfsSeq_t theSeq = IsAuthEnabled() ?
                mLookupOp.seq : kfsSeq_t(-1);
            mLookupOp.seq = -1; // Mark as not in flight for reset connection.
            ResetConnection();
            mLookupOp.seq = theSeq;
            mServerLocation = inLocation;
            mRpcFormat      = inRpcFormat;
            mConnPtr.swap(inConnPtr);
            mConnPtr->SetOwningKfsCallbackObj(this);
            CancelVrPrimaryCheck();
            const bool kVrPrimaryFlag = true;
            EnsureConnectedSelf(0, 0, kVrPrimaryFlag);
            return;
        }
        assert(0 <= mMetaVrNodesActiveCount);
        mMetaVrNodesActiveCount--;
        if (mMetaVrNodesActiveCount <= 0 &&
                ResolverList::IsEmpty(mResolverReqsPtr)) {
            CancelVrPrimaryCheck();
            Fail(
                mLookupOp.status < 0 ? mLookupOp.status : -EIO,
                mLookupOp.statusMsg
            );
        }
    }
    void Resolve(
        const string& inHostName)
    {
        if (! mResolverPtr) {
            mResolverPtr = new Resolver(*mNetManagerPtr);
            const int theStatus = mResolverPtr->Start();
            if (0 != theStatus) {
                KFS_LOG_STREAM_FATAL << "failed to start resolver:" <<
                    " status: " << theStatus <<
                    " "         << QCUtils::SysError(-theStatus) <<
                KFS_LOG_EOM;
                MsgLogger::Stop();
                abort();
            }
        }
        ResolverReq& theReq = *(new ResolverReq(inHostName, *this));
        ResolverList::PushBack(mResolverReqsPtr, theReq);
        mResolverPtr->Enqueue(theReq);
    }
    void Resolved(
        ResolverReq& inReq)
    {
        MetaCheckVrPrimaryChecker* theListPtr[1];
        MetaVrList::Init(theListPtr);
        if (0 == inReq.GetStatus()) {
            const string&                   theHost = inReq.GetHostName();
            const ResolverReq::IpAddresses& theIps  = inReq.GetIps();
            if (! theIps.empty()) {
                for (MetaLocations::const_iterator
                        theIt = mMetaLocations.begin();
                        mMetaLocations.end() != theIt;
                        ++theIt) {
                    if (theIt->hostname != theHost) {
                        continue;
                    }
                    for (ResolverReq::IpAddresses::const_iterator
                            theIpIt = theIps.begin();
                            theIps.end() != theIpIt;
                            ++theIpIt) {
                        ServerLocation const theLocation(*theIpIt, theIt->port);
                        MetaVrList::Iterator             theIt(mMetaVrListPtr);
                        const MetaCheckVrPrimaryChecker* thePtr;
                        while ((thePtr = theIt.Next())) {
                            if (thePtr->GetLocation() == theLocation) {
                                break;
                            }
                        }
                        if (! thePtr) {
                            const bool kLocationResolvedFlag = true;
                            MetaVrList::PushBack(theListPtr,
                                *(new MetaCheckVrPrimaryChecker(
                                    theLocation, *this, kLocationResolvedFlag)));
                        }
                    }
                }
            }
        }
        inReq.Delete(mResolverReqsPtr);
        if (mMetaVrNodesActiveCount <= 0 && MetaVrList::IsEmpty(theListPtr)) {
            assert(! mConnPtr);
            if (0 <= mLookupOp.status) {
                mLookupOp.status    = inReq.GetStatus();
                mLookupOp.statusMsg = inReq.GetStatusMsg();
                if (0 <= mLookupOp.status) {
                    mLookupOp.lastError = -mLookupOp.status;
                }
                mLookupOp.status = -ENETUNREACH;
            }
            SetVrPrimary(mConnPtr, ServerLocation(), kRpcFormatUndef);
            return;
        }
        MetaCheckVrPrimaryChecker* thePtr;
        while ((thePtr = MetaVrList::PopFront(theListPtr))) {
            MetaVrList::PushBack(mMetaVrListPtr, *thePtr);
            mMetaVrNodesCount++;
            mMetaVrNodesActiveCount++;
            thePtr->Connect();
        }
    }
    bool ConnectToVrPrimary(
        const KfsOp* inLastOpPtr)
    {
        if (MetaVrList::IsEmpty(mMetaVrListPtr) && mMetaLocations.empty()) {
            return false;
        }
        if (inLastOpPtr) {
            if (mPendingOpQueue.empty()) {
                assert(! "vr connect: invalid op -- queue is empty");
            } else {
                OpQueueEntry& theEntry = mPendingOpQueue.rbegin()->second;
                if (theEntry.mOpPtr == inLastOpPtr) {
                    if (0 < theEntry.mRetryCount) {
                        theEntry.mVrConnectPendingFlag = true;
                    }
                } else {
                    const OpQueue::iterator theIt =
                        mPendingOpQueue.find(inLastOpPtr->seq);
                    if (theIt != mPendingOpQueue.end() &&
                            theIt->second.mOpPtr == inLastOpPtr) {
                        if (0 < theIt->second.mRetryCount) {
                            theIt->second.mVrConnectPendingFlag = true;
                        }
                    } else {
                        assert(! "vr connect: op is not in the pending queue");
                    }
                }
            }
        }
        if (0 < mMetaVrNodesActiveCount ||
                ! ResolverList::IsEmpty(mResolverReqsPtr)) {
            return true;
        }
        InitConnect();
        // Ref self to ensure that "this" is still around after the end of the
        // of the loop to ensure that iterator has valid list head pointer.
        StRef theRef(*this);
        for (MetaHostNames::const_iterator theIt = mMetaHostNames.begin();
                mMetaHostNames.end() != theIt;
                ++theIt) {
            Resolve(*theIt);
        }
        // Unwind recursion by setting active count to the total node count
        // prior to launching requests.
        mMetaVrNodesActiveCount = mMetaVrNodesCount;
        MetaVrList::Iterator       theIt(mMetaVrListPtr);
        MetaCheckVrPrimaryChecker* thePtr;
        while ((thePtr = theIt.Next())) {
            thePtr->Connect();
        }
        return true;
    }
    void CancelVrPrimaryCheck()
    {
        MetaVrList::Iterator       theIt(mMetaVrListPtr);
        MetaCheckVrPrimaryChecker* thePtr;
        while ((thePtr = theIt.Next())) {
            thePtr->Reset();
            if (thePtr->IsLocationResolved()) {
                MetaVrList::Remove(mMetaVrListPtr, *thePtr);
                delete thePtr;
                mMetaVrNodesCount--;
            }
        }
        mMetaVrNodesActiveCount = 0;
        ResolverReq* theReqPtr;
        while ((theReqPtr = ResolverList::Front(mResolverReqsPtr))) {
            theReqPtr->Delete(mResolverReqsPtr);
        }
    }
    bool EnqueueSelf(
        KfsOp*    inOpPtr,
        OpOwner*  inOwnerPtr,
        IOBuffer* inBufferPtr,
        int       inRetryCount)
    {
        if (! inOpPtr) {
            return false;
        }
        mIdleTimeoutFlag = false;
        SetMaxWaitTime(*inOpPtr);
        inOpPtr->seq = mNextSeqNum++;
        const bool theResetTimerFlag = mPendingOpQueue.empty();
        pair<OpQueue::iterator, bool> const theRes =
            mPendingOpQueue.insert(make_pair(
                inOpPtr->seq, OpQueueEntry(inOpPtr, inOwnerPtr, inBufferPtr)
            ));
        if (! theRes.second || ! IsConnected() || IsAuthInFlight()) {
            return theRes.second;
        }
        if (mMaxOneOutstandingOpFlag) {
            if (mOutstandingOpPtr) {
                return theRes.second;
            }
            mOutstandingOpPtr = &(mPendingOpQueue.begin()->second);
        }
        Request(mOutstandingOpPtr ? *mOutstandingOpPtr : theRes.first->second,
            theResetTimerFlag || mOutstandingOpPtr, inRetryCount);
        return theRes.second;
    }
    void EnqueueAuth(
        KfsOp& inOp)
    {
        assert(! mOutstandingOpPtr && mConnPtr && ! mConnPtr->IsWriteReady());
        SetMaxWaitTime(inOp);
        pair<OpQueue::iterator, bool> const theRes =
            mPendingOpQueue.insert(make_pair(
                inOp.seq, OpQueueEntry(&inOp, this, 0)
            ));
        if (! theRes.second || theRes.first != mPendingOpQueue.begin()) {
            KFS_LOG_STREAM_FATAL << "invalid auth. enqueue attempt:" <<
                " duplicate seq. number: " << theRes.second <<
            KFS_LOG_EOM;
            MsgLogger::Stop();
            abort();
        }
        const bool kResetTimerFlag = true;
        Request(theRes.first->second, kResetTimerFlag, mRetryCount);
    }
    void Request(
        OpQueueEntry& inEntry,
        bool          inResetTimerFlag,
        int           inRetryCount,
        bool          inFlushFlag = true)
    {
        KfsOp& theOp = *inEntry.mOpPtr;
        theOp.shortRpcFormatFlag = mRpcFormat == kRpcFormatShort;
        {
            if (IsAuthEnabled()) {
                theOp.extraHeaders = 0;
            } else {
                theOp.extraHeaders = theOp.shortRpcFormatFlag ?
                    &mCommonShortHeaders : &mCommonHeaders;
            }
            ReqOstream theStream(mOstream.Set(mConnPtr->GetOutBuffer()));
            theOp.Request(theStream);
            mOstream.Reset();
            theOp.extraHeaders = 0;
        }
        if (theOp.contentLength > 0) {
            if (theOp.contentBuf && theOp.contentBufLen > 0) {
                assert(theOp.contentBufLen >= theOp.contentLength);
                mConnPtr->Write(theOp.contentBuf, theOp.contentLength,
                    inResetTimerFlag);
            } else if (inEntry.mBufferPtr) {
                assert(size_t(inEntry.mBufferPtr->BytesConsumable()) >=
                    theOp.contentLength);
                mConnPtr->WriteCopy(inEntry.mBufferPtr, theOp.contentLength,
                    inResetTimerFlag);
            }
        }
        for (; ;) {
            ReqOstream theStream(mOstream.Set(mConnPtr->GetOutBuffer()));
            if (! theOp.NextRequest(mNextSeqNum, theStream)) {
                break;
            }
            mNextSeqNum++;
            mOstream.Reset();
        }
        mOstream.Reset();
        // Start the timer.
        inEntry.mTime       = Now();
        inEntry.mRetryCount = inRetryCount;
        mPendingBytesSend = mConnPtr->GetOutBuffer().BytesConsumable();
        if (inFlushFlag) {
            mConnPtr->SetInactivityTimeout(mOpTimeoutSec);
            mConnPtr->Flush(inResetTimerFlag);
        }
    }
    void SubmitPending()
    {
        const bool kFlushFlag             = false;
        bool       theFlushConnectionFlag = false;
        for (OpQueue::iterator theIt = mPendingOpQueue.begin();
                ! mOutstandingOpPtr && theIt != mPendingOpQueue.end();
                ++theIt) {
            if (mMaxOneOutstandingOpFlag) {
                mOutstandingOpPtr = &(theIt->second);
            }
            Request(theIt->second, kFlushFlag,
                theIt->second.mRetryCount, kFlushFlag);
            theFlushConnectionFlag = true;
        }
        if (theFlushConnectionFlag) {
            mConnPtr->SetInactivityTimeout(mOpTimeoutSec);
            mConnPtr->Flush();
        }
    }
    void HandleResponse(
       IOBuffer& inBuffer)
    {
        for (; ;) {
            if (! mReadHeaderDoneFlag && ! ReadHeader(inBuffer)) {
                break;
            }
            if (mContentLength > inBuffer.BytesConsumable()) {
                if (! mInFlightOpPtr) {
                    // Discard content.
                    const int theCount = inBuffer.Consume(mContentLength);
                    mContentLength -= theCount;
                    mStats.mBytesReceivedCount += theCount;
                }
                if (mConnPtr) {
                    mConnPtr->SetMaxReadAhead(max(int(kMaxReadAhead),
                        mContentLength - inBuffer.BytesConsumable()));
                }
                break;
            }
            // Get ready for next op.
            if (mConnPtr) {
                mConnPtr->SetMaxReadAhead(kMaxReadAhead);
            }
            mReadHeaderDoneFlag = false;
            TransmitAck();
            if (! mInFlightOpPtr) {
                mStats.mBytesReceivedCount += inBuffer.Consume(mContentLength);
                mContentLength = 0;
                mProperties.clear();
                // Don't rely on compiler to properly handle tail recursion,
                // use for loop instead.
                continue;
            }
            assert(&mCurOpIt->second == mInFlightOpPtr);
            assert(mInFlightOpPtr->mOpPtr);
            KfsOp&          theOp     = *mInFlightOpPtr->mOpPtr;
            IOBuffer* const theBufPtr = mInFlightOpPtr->mBufferPtr;
            mInFlightOpPtr = 0;
            theOp.ParseResponseHeader(mProperties);
            mProperties.clear();
            if (mContentLength > 0) {
                mStats.mBytesReceivedCount +=
                    min(mContentLength, inBuffer.BytesConsumable());
                if (theBufPtr) {
                    IOBuffer theBuf;
                    theBuf.MoveSpaceAvailable(theBufPtr, mContentLength);
                    theBuf.Clear();
                    const int kMaxInt = ~(int(1) << (sizeof(int) * 8 - 1));
                    theBuf.MoveSpaceAvailable(theBufPtr, kMaxInt);
                    QCVERIFY(mContentLength ==
                        theBufPtr->MoveSpace(&inBuffer, mContentLength)
                    );
                    theBufPtr->Move(&theBuf);
                } else {
                    IOBuffer theBuf;
                    QCVERIFY(mContentLength ==
                        theBuf.MoveSpace(&inBuffer, mContentLength)
                    );
                    if (mInFlightRecvBufPtr) {
                        theOp.AttachContentBuf(
                            mInFlightRecvBufPtr, mContentLength);
                        mInFlightRecvBufPtr = 0;
                    }
                }
                mContentLength = 0;
            }
            mRetryCount = 0;
            HandleOp(mCurOpIt);
        }
    }
    bool ReadHeaderSelf(
        IOBuffer&   inBuffer,
        Properties& inProperties,
        kfsSeq_t&   outOpSeq,
        RpcFormat&  ioRpcFormat,
        int&        outContentLength,
        bool&       outErrorFlag)
    {
        outErrorFlag = false;
        const int theIdx = inBuffer.IndexOf(0, "\r\n\r\n");
        if (theIdx < 0) {
            if (mMaxRpcHeaderLength < inBuffer.BytesConsumable()) {
               KFS_LOG_STREAM_ERROR << mLogPrefix <<
                    "error: " << mServerLocation <<
                    ": exceeded max. response header size: " <<
                    mMaxRpcHeaderLength << "; got " <<
                    inBuffer.BytesConsumable() << " resetting connection" <<
                KFS_LOG_EOM;
                outErrorFlag = true;
            }
            return false;
        }
        const int  theHdrLen    = theIdx + 4;
        const char theSeparator = ':';
        inProperties.clear();
        IOBuffer::iterator const theIt = inBuffer.begin();
        if (theIt != inBuffer.end() && theHdrLen <= theIt->BytesConsumable()) {
            inProperties.loadProperties(
                theIt->Consumer(), (size_t)theHdrLen, theSeparator);
        } else {
            inProperties.loadProperties(
                mIstream.Set(inBuffer, theHdrLen), theSeparator);
            mIstream.Reset();
        }
        mStats.mBytesReceivedCount += inBuffer.Consume(theHdrLen);
        if (kRpcFormatUndef == ioRpcFormat) {
            ioRpcFormat = inProperties.getValue("c") ?
                kRpcFormatShort : kRpcFormatLong;
        }
        inProperties.setIntBase(kRpcFormatShort == ioRpcFormat ? 16 : 10);
        outOpSeq = inProperties.getValue(
            kRpcFormatShort == ioRpcFormat ? "c" : "Cseq", kfsSeq_t(-1));
        outContentLength = inProperties.getValue(
            kRpcFormatShort == ioRpcFormat ? "l" : "Content-length", 0);
        return true;
    }
    bool ReadHeader(
        IOBuffer& inBuffer)
    {
        kfsSeq_t theOpSeq     = -1;
        bool     theErrorFlag = false;
        if (! ReadHeaderSelf(
                    inBuffer,
                    mProperties,
                    theOpSeq,
                    mRpcFormat,
                    mContentLength,
                    theErrorFlag)) {
            if (theErrorFlag) {
                Reset();
                EnsureConnected();
            }
            return false;
        }
        mReadHeaderDoneFlag = true;
        if (mContentLength > mMaxContentLength) {
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                "error: " << mServerLocation <<
                ": exceeded max. response content length: " << mContentLength <<
                " > " << mMaxContentLength <<
                " seq: " << theOpSeq <<
                ", resetting connection" <<
            KFS_LOG_EOM;
            Reset();
            EnsureConnected();
            return false;
        }
        mCurOpIt = mPendingOpQueue.find(theOpSeq);
        mInFlightOpPtr =
            mCurOpIt != mPendingOpQueue.end() ? &mCurOpIt->second : 0;
        if (! mInFlightOpPtr) {
            // Discard canceled op reply.
            KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                "no operation found with seq: " << theOpSeq <<
                ", discarding response " <<
                " content length: " << mContentLength <<
            KFS_LOG_EOM;
            const int theCount = inBuffer.Consume(mContentLength);
            mContentLength -= theCount;
            mStats.mBytesReceivedCount += theCount;
            return true;
        }
        if (mInFlightOpPtr->mOpPtr) {
            mInFlightOpPtr->mOpPtr->shortRpcFormatFlag =
                kRpcFormatShort == mRpcFormat;
        }
        if (mOutstandingOpPtr && mOutstandingOpPtr != mInFlightOpPtr) {
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                "error: "     << mServerLocation <<
                " seq: "      << theOpSeq <<
                " op:"
                " expected: " << static_cast<const void*>(mOutstandingOpPtr) <<
                " actual: "   << static_cast<const void*>(mInFlightOpPtr) <<
                " seq:" <<
                " expected: " << mOutstandingOpPtr->mOpPtr->seq <<
                " actual: "   << mInFlightOpPtr->mOpPtr->seq <<
                ", resetting connection" <<
            KFS_LOG_EOM;
            Reset();
            EnsureConnected();
            return false;
        }
        if (mContentLength <= 0) {
            return true;
        }
        if (mInFlightOpPtr->mBufferPtr) {
            if (inBuffer.UseSpaceAvailable(
                    mInFlightOpPtr->mBufferPtr, mContentLength) <= 0) {
                // Move the payload, if any, to the beginning of the new buffer.
                inBuffer.MakeBuffersFull();
            }
            return true;
        }
        KfsOp& theOp = *mInFlightOpPtr->mOpPtr;
        assert(! mInFlightRecvBufPtr);
        if (theOp.contentLength <= 0) {
            theOp.EnsureCapacity(size_t(mContentLength));
        } else {
            mInFlightRecvBufPtr = new char [mContentLength + 1];
            mInFlightRecvBufPtr[mContentLength] = 0;
        }
        IOBuffer theBuf;
        theBuf.Append(IOBufferData(
            IOBufferData::IOBufferBlockPtr(
                mInFlightRecvBufPtr ? mInFlightRecvBufPtr : theOp.contentBuf,
                DoNotDeallocate()
            ),
            mContentLength,
            0,
            0
        ));
        inBuffer.UseSpaceAvailable(&theBuf, mContentLength);
        return true;
    }
    void CancelInFlightOp()
    {
        if (! mInFlightOpPtr) {
            return;
        }
        if (0 < mContentLength && mConnPtr) {
            // Detach shared buffers, if any.
            IOBuffer& theBuf = mConnPtr->GetInBuffer();
            mContentLength -= theBuf.BytesConsumable();
            assert(0 <= mContentLength);
            theBuf.Clear();
        }
        delete [] mInFlightRecvBufPtr;
        mInFlightRecvBufPtr = 0;
        mInFlightOpPtr = 0;
    }
    void TransmitAck()
    {
        if (! mConnPtr) {
            return;
        }
        const Properties::String* const theAckPtr = mProperties.getValue(
            kRpcFormatShort == mRpcFormat ? "a" : "Ack");
        if (! theAckPtr) {
            return;
        }
        // Transmit acknowledgement of response reception.
        IOBuffer& theBuf = mConnPtr->GetOutBuffer();
        if (kRpcFormatShort == mRpcFormat) {
            theBuf.CopyIn("ACK\r\na:", 7);
        } else {
            theBuf.CopyIn("ACK\r\nAck: ", 10);
        }
        theBuf.CopyIn(theAckPtr->data(), theAckPtr->size());
        if (IsAuthEnabled()) {
            theBuf.CopyIn("\r\n\r\n", 4);
        } else {
            const string& theHeaders = kRpcFormatShort == mRpcFormat ?
                mCommonShortHeaders : mCommonHeaders;
            theBuf.CopyIn("\r\n", 2);
            theBuf.CopyIn(theHeaders.data(), (int)theHeaders.size());
            theBuf.CopyIn("\r\n", 2);
        }
        mConnPtr->Flush();
    }
    bool IsAuthEnabled() const
        { return (mAuthContextPtr && mAuthContextPtr->IsEnabled()); }
    bool IsPskAuth() const
        { return (IsAuthEnabled() && ! mKeyData.empty()); }
    int Connect(
        const ServerLocation& inServerLocation,
        NetConnectionPtr&     inConnPtr,
        KfsCallbackObj&       inCallbackObj,
        string*               inErrMsgPtr = 0)
    {
        mStats.mConnectCount++;
        const bool theNonBlockingFlag = true;
        TcpSocket& theSocket          = *(new TcpSocket());
        const int theErr              = theSocket.Connect(
            inServerLocation, theNonBlockingFlag);
        if (theErr && theErr != -EINPROGRESS) {
            if (inErrMsgPtr) {
                *inErrMsgPtr = QCUtils::SysError(-theErr);
            }
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                "failed to connect to server " << inServerLocation <<
                " : " << QCUtils::SysError(-theErr) <<
            KFS_LOG_EOM;
            delete &theSocket;
            mStats.mConnectFailureCount++;
            return theErr;
        }
        inConnPtr.reset(new NetConnection(&theSocket, &inCallbackObj));
        inConnPtr->EnableReadIfOverloaded();
        if (theErr) {
            inConnPtr->SetDoingNonblockingConnect();
        }
        inConnPtr->SetMaxReadAhead(kMaxReadAhead);
        inConnPtr->SetInactivityTimeout(mOpTimeoutSec);
        // Add connection to the poll vector
        mNetManagerPtr->AddConnection(inConnPtr);
        return 0;
    }
    void InitConnect()
    {
        mDataReceivedFlag = false;
        mDataSentFlag     = false;
        mAllDataSentFlag  = true;
        mIdleTimeoutFlag  = false;
        ResetConnection();
    }
    void EnsureConnected(
        string*      inErrMsgPtr = 0,
        const KfsOp* inLastOpPtr = 0)
    {
        if (mSleepingFlag || IsConnected() || ConnectToVrPrimary(inLastOpPtr)) {
            return;
        }
        assert(mLookupOp.seq < 0 && mAuthOp.seq < 0);
        InitConnect();
        const int theError = Connect(
            mServerLocation, mConnPtr, *this, inErrMsgPtr);
        if (0 != theError) {
            RetryConnect(0, theError);
            return;
        }
        const bool kVrPrimaryFlag = false;
        EnsureConnectedSelf(inErrMsgPtr, inLastOpPtr, kVrPrimaryFlag);
    }
    void EnsureConnectedSelf(
        string*      inErrMsgPtr,
        const KfsOp* inLastOpPtr,
        bool         inVrPrimaryFlag)
    {
        KFS_LOG_STREAM_DEBUG << mLogPrefix <<
            "connecting to server: " << mServerLocation <<
            " auth: " << (IsAuthEnabled() ?
                (IsPskAuth() ? "psk" : "on") : "off") <<
        KFS_LOG_EOM;
        mSslShutdownInProgressFlag = false;
        if (IsPskAuth()) {
            KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                "psk key:"
                " size: " << mKeyData.size() <<
                " id: "   << mKeyId <<
            KFS_LOG_EOM;
            string    theErrMsg;
            const int theStatus = mAuthContextPtr->StartSsl(
                *mConnPtr,
                mKeyId.c_str(),
                mKeyData.data(),
                (int)mKeyData.size(),
                &theErrMsg
            );
            if (theStatus != 0) {
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "failed to start ssl:"
                    " error: " << theStatus <<
                    " "        << theErrMsg <<
                KFS_LOG_EOM;
                Fail(theStatus, theErrMsg);
                if (inErrMsgPtr) {
                    *inErrMsgPtr = theErrMsg;
                }
                return;
            }
            if (mShutdownSslFlag && mConnPtr->IsGood()) {
                mSslShutdownInProgressFlag = true;
                const int theStatus = mConnPtr->Shutdown();
                if (theStatus != 0) {
                    mSslShutdownInProgressFlag = false;
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "ssl shutdown failure: " <<
                            theStatus <<
                    KFS_LOG_EOM;
                    // Assume communication failure.
                    mStats.mConnectFailureCount++;
                    RetryConnect(0, theStatus < 0 ? theStatus : -EIO);
                    return;
                }
            }
            mSessionExpirationTime = mKeyExpirationTime;
            mSessionKeyId          = mKeyId;
            mSessionKeyData        = mKeyData;
            mNonAuthRetryCount     = 0;
        } else {
            mSessionKeyId          = string();
            mSessionKeyData        = mSessionKeyId;
            mSessionExpirationTime = Now() + kMaxSessionTimeout;
            if (IsAuthEnabled()) {
                if (! inVrPrimaryFlag) {
                    assert(! IsAuthInFlight());
                    InitHelloLookupOp();
                }
                mNextSeqNum++; // Leave one slot for mAuthOp
            } else {
                mNonAuthRetryCount = 0;
            }
        }
        const kfsSeq_t theLookupSeq = mLookupOp.seq;
        RetryAll(inLastOpPtr);
        if (! mConnPtr) {
            ResetConnection();
        } else if (0 <= theLookupSeq && theLookupSeq == mLookupOp.seq) {
            if (inVrPrimaryFlag) {
                OpDone(&mLookupOp, false, 0);
            } else {
                EnqueueAuth(mLookupOp);
            }
        }
    }
    void RetryAll(
        const KfsOp* inLastOpPtr = 0)
    {
        if (mPendingOpQueue.empty()) {
            return;
        }
        CancelInFlightOp();
        mNextSeqNum += 1000; // For debugging to see retries.
        QueueStack::iterator const theIt =
            mQueueStack.insert(mQueueStack.end(), OpQueue());
        OpQueue& theQueue = *theIt;
        theQueue.swap(mPendingOpQueue);
        for (OpQueue::iterator theIt = theQueue.begin();
                theIt != theQueue.end();
                ++theIt) {
            OpQueueEntry& theEntry = theIt->second;
            if (! theEntry.mOpPtr) {
                continue;
            }
            if (theEntry.mRetryCount > mMaxRetryCount) {
                mStats.mOpsTimeoutCount++;
                theEntry.mOpPtr->status = kErrorMaxRetryReached;
                if (0 == theEntry.mOpPtr->lastError) {
                    theEntry.mOpPtr->lastError = -EIO;
                }
                theEntry.Done();
            } else {
                if (inLastOpPtr != theEntry.mOpPtr &&
                        0 < theEntry.mRetryCount) {
                    if (theEntry.mVrConnectPendingFlag) {
                        theEntry.mVrConnectPendingFlag = false;
                    } else {
                        mStats.mOpsRetriedCount++;
                    }
                }
                EnqueueSelf(theEntry.mOpPtr, theEntry.mOwnerPtr,
                    theEntry.mBufferPtr, theEntry.mRetryCount);
                theEntry.Clear();
            }
        }
        mQueueStack.erase(theIt);
    }
    void ResetConnection()
    {
        CancelInFlightOp();
        mOutstandingOpPtr = 0;
        if (mConnPtr) {
            mConnPtr->Close();
            mConnPtr->GetInBuffer().Clear();
            mConnPtr->SetOwningKfsCallbackObj(0);
            mConnPtr.reset();
            mDisconnectCount++;
        }
        if (0 <= mLookupOp.seq) {
            Cancel(&mLookupOp, this);
            mLookupOp.seq = -1;
        }
        if (0 <= mAuthOp.seq) {
            Cancel(&mAuthOp, this);
            mAuthOp.seq = -1;
        }
        mReadHeaderDoneFlag        = false;
        mContentLength             = 0;
        mSslShutdownInProgressFlag = false;
    }
    void HandleOp(
        OpQueue::iterator inIt,
        bool              inCanceledFlag = false)
    {
        if (inCanceledFlag) {
            mStats.mOpsCancelledCount++;
        }
        const bool theScheduleNextOpFlag = mOutstandingOpPtr == &inIt->second;
        if (theScheduleNextOpFlag) {
            mOutstandingOpPtr = 0;
        }
        if (! inCanceledFlag) {
            if (IsMetaLogWriteOrVrError(inIt->second.mOpPtr->status)) {
                if (++mMetaLogWriteRetryCount < mMaxMetaLogWriteRetryCount) {
                    KFS_LOG_STREAM_INFO << mLogPrefix <<
                        "seq: "     << inIt->second.mOpPtr->seq <<
                        " status: " << inIt->second.mOpPtr->status <<
                        " "         << inIt->second.mOpPtr->statusMsg <<
                        " retry: "  << mMetaLogWriteRetryCount <<
                        " of "      << mMaxMetaLogWriteRetryCount <<
                        " "         << inIt->second.mOpPtr->Show() <<
                    KFS_LOG_EOM;
                    const int theStatus = inIt->second.mOpPtr->status;
                    inIt->second.mOpPtr->status = 0;
                    inIt->second.mOpPtr->statusMsg.clear();
                    const int kRetryIncrement = 0;
                    RetryConnect(mOutstandingOpPtr, theStatus, kRetryIncrement);
                    return;
                }
            } else if (0 < mMetaLogWriteRetryCount &&
                    &mLookupOp != inIt->second.mOpPtr &&
                    &mAuthOp != inIt->second.mOpPtr) {
                mMetaLogWriteRetryCount = 0;
            }
        }
        if (&inIt->second == mInFlightOpPtr) {
            CancelInFlightOp();
        }
        OpQueueEntry theOpEntry = inIt->second;
        mPendingOpQueue.erase(inIt);
        const int thePrevRefCount = GetRefCount();
        theOpEntry.OpDone(inCanceledFlag);
        if (! mOutstandingOpPtr &&
                theScheduleNextOpFlag && thePrevRefCount <= GetRefCount() &&
                ! mPendingOpQueue.empty() && IsConnected()) {
            mOutstandingOpPtr = &(mPendingOpQueue.begin()->second);
            const bool kResetTimerFlag = true;
            Request(*mOutstandingOpPtr, kResetTimerFlag,
                mOutstandingOpPtr->mRetryCount);
        }
    }
    void Cancel(
        OpQueue::iterator inIt)
        { HandleOp(inIt, true); }
    virtual void Timeout()
    {
        if (mSleepingFlag) {
            mNetManagerPtr->UnRegisterTimeoutHandler(this);
            mSleepingFlag = false;
        }
        if (mPendingOpQueue.empty()) {
            return;
        }
        EnsureConnected();
    }
    void RetryConnect(
        OpQueueEntry* inOutstandingOpPtr,
        int           inError,
        int           inRetryIncrement = 1)
    {
        if (mSleepingFlag) {
            return;
        }
        CancelInFlightOp();
        if (mRetryCount < mMaxRetryCount &&
                (! mAuthContextPtr ||
                mAuthFailureCount < mAuthContextPtr->GetMaxAuthRetryCount()) &&
                (! mRetryConnectOnlyFlag ||
                (! mDataSentFlag && ! mDataReceivedFlag) ||
                IsAuthInFlight())) {
            mRetryCount += inRetryIncrement;
            if (! IsAuthInFlight()) {
                mNonAuthRetryCount = mRetryCount;
            }
            ResetConnection();
            if (0 < mTimeSecBetweenRetries) {
                KFS_LOG_STREAM_INFO << mLogPrefix <<
                    "retry attempt " << mRetryCount <<
                    " of " << mMaxRetryCount <<
                    ", will retry " << mPendingOpQueue.size() <<
                    " pending operation(s) in " << mTimeSecBetweenRetries <<
                    " seconds" <<
                KFS_LOG_EOM;
                mStats.mSleepTimeSec += mTimeSecBetweenRetries;
                SetTimeoutInterval(mTimeSecBetweenRetries * 1000, true);
                mSleepingFlag = true;
                mNetManagerPtr->RegisterTimeoutHandler(this);
            } else {
                Timeout();
            }
        } else if (IsAuthInFlight()) {
            OpQueue::iterator const theIt = mPendingOpQueue.begin();
            assert(
                theIt != mPendingOpQueue.end() &&
                theIt->second.mOpPtr ==
                    (0 <= mLookupOp.seq ?
                        static_cast<KfsOp*>(&mLookupOp) :
                        static_cast<KfsOp*>(&mAuthOp))
            );
            const bool kAllowRetryFlag = false;
            HandleSingleOpTimeout(
                theIt, kAllowRetryFlag, kErrorMaxRetryReached, inError);
        } else if (inOutstandingOpPtr && ! mFailAllOpsOnOpTimeoutFlag &&
                ! mPendingOpQueue.empty() &&
                &(mPendingOpQueue.begin()->second) == inOutstandingOpPtr) {
            const bool kAllowRetryFlag = true;
            HandleSingleOpTimeout(mPendingOpQueue.begin(), kAllowRetryFlag,
                mAuthFailureCount ? -EPERM : kErrorMaxRetryReached, inError);
        } else {
            const int theStatus = 0 < mAuthFailureCount ?
                -EPERM : kErrorMaxRetryReached;
            QueueStack::iterator const theIt =
                mQueueStack.insert(mQueueStack.end(), OpQueue());
            OpQueue& theQueue = *theIt;
            theQueue.swap(mPendingOpQueue);
            for (OpQueue::iterator theIt = theQueue.begin();
                    theIt != theQueue.end();
                    ++theIt) {
                if (! theIt->second.mOpPtr) {
                    continue;
                }
                theIt->second.mOpPtr->status    = theStatus;
                theIt->second.mOpPtr->lastError = inError;
                theIt->second.Done();
            }
            mQueueStack.erase(theIt);
        }
    }
    void HandleSingleOpTimeout(
        OpQueue::iterator inIt,
        bool              inAllowRetryFlag,
        int               inStatus,
        int               inError)
    {
        OpQueueEntry& theEntry = inIt->second;
        if (inAllowRetryFlag && theEntry.mRetryCount < mMaxRetryCount) {
            theEntry.mRetryCount++;
        } else {
            theEntry.mOpPtr->status    = inStatus;
            theEntry.mOpPtr->lastError = inError;
            const int thePrevRefCount  = GetRefCount();
            HandleOp(inIt);
            if (thePrevRefCount > GetRefCount()) {
                return;
            }
        }
        if (! mPendingOpQueue.empty()) {
            EnsureConnected();
        }
    }
    void OpsTimeout()
    {
        if (mOpTimeoutSec <= 0 || ! IsConnected()) {
            return;
        }
        const time_t theNow = Now();
        if (IsAuthInFlight()) {
            OpQueue::iterator const theIt = mPendingOpQueue.begin();
            assert(
                theIt != mPendingOpQueue.end() &&
                theIt->second.mOpPtr ==
                    (0 <= mLookupOp.seq ?
                        static_cast<KfsOp*>(&mLookupOp) :
                        static_cast<KfsOp*>(&mAuthOp))
            );
            if (theIt->second.mTime + mOpTimeoutSec < theNow) {
                const OpQueueEntry& theEntry = theIt->second;
                KFS_LOG_STREAM_INFO << mLogPrefix <<
                    "auth. op timed out:"
                    " seq: "               << theEntry.mOpPtr->seq <<
                    " "                    << theEntry.mOpPtr->Show() <<
                    " wait time: "         << (theNow - theEntry.mTime) <<
                    " pending ops: "       << mPendingOpQueue.size() <<
                    " resetting connecton" <<
                KFS_LOG_EOM;
                RetryConnect(0, -ETIMEDOUT);
            }
            return;
        }
        // Timeout ops waiting for response.
        // The ops in the queue are ordered by op seq. number.
        // The code ensures (initial seq. number in ctor) that seq. numbers
        // never wrap around, and are monotonically increase, so that the last
        // (re)queued operation seq. number is always the largest.
        // First move all timed out ops into the temporary queue, then process
        // the temporary queue. This is less error prone than dealing with
        // completion changing mPendingOpQueue while iterating.
        QueueStack::iterator theStIt       = mQueueStack.end();
        time_t               theExpireTime = theNow - mOpTimeoutSec;
        const bool           theMaxOneOutstandingOpFlag =
            mMaxOneOutstandingOpFlag;
        for (OpQueue::iterator theIt = mPendingOpQueue.begin();
                theIt != mPendingOpQueue.end() &&
                theIt->second.mTime < theExpireTime; ) {
            OpQueueEntry& theEntry = theIt->second;
            assert(theEntry.mOpPtr);
            if (&theEntry == mInFlightOpPtr) {
                CancelInFlightOp();
            }
            if (mResetConnectionOnOpTimeoutFlag) {
                KFS_LOG_STREAM_INFO << mLogPrefix <<
                    "op timed out: seq: "  << theEntry.mOpPtr->seq <<
                    " "                    << theEntry.mOpPtr->Show() <<
                    " retry count: "       << theEntry.mRetryCount <<
                    " wait time: "         << (theNow - theEntry.mTime) <<
                    " pending ops: "       << mPendingOpQueue.size() <<
                    " resetting connecton" <<
                KFS_LOG_EOM;
                // Increment the retry count only for the op that timed out.
                // This mode assumes all other ops were blocked by the first
                // one.
                Reset();
                if (mFailAllOpsOnOpTimeoutFlag) {
                    // Fail all ops.
                    assert(! mOutstandingOpPtr && ! mInFlightOpPtr);
                    theStIt = mQueueStack.insert(mQueueStack.end(), OpQueue());
                    theStIt->swap(mPendingOpQueue);
                    break;
                }
                const bool kAllowRetryFlag = true;
                HandleSingleOpTimeout(
                    theIt, kAllowRetryFlag, kErrorMaxRetryReached, -ETIMEDOUT);
                return;
            }
            if (theStIt == mQueueStack.end()) {
                theStIt = mQueueStack.insert(mQueueStack.end(), OpQueue());
            }
            theStIt->insert(*theIt);
            mPendingOpQueue.erase(theIt++);
            if (&theEntry == mOutstandingOpPtr) {
                break;
            }
        }
        if (theStIt == mQueueStack.end()) {
            return;
        }
        int theStatus         = kErrorMaxRetryReached;
        int theRetryIncrement = 1;
        for (OpQueue::iterator theIt = theStIt->begin();
                theIt != theStIt->end();
                ++theIt) {
            const int theCurStatus = theStatus;
            if (theMaxOneOutstandingOpFlag) {
                theStatus         = kErrorRequeueRequired;
                theRetryIncrement = 0;
            }
            OpQueueEntry& theEntry = theIt->second;
            if (! theEntry.mOpPtr) {
                continue;
            }
            KFS_LOG_STREAM_INFO << mLogPrefix <<
                "op " << (theStatus == kErrorRequeueRequired ?
                    "re-queue" : "timed out") <<
                " seq: "              << theEntry.mOpPtr->seq <<
                " "                   << theEntry.mOpPtr->Show() <<
                " retry count: "      << theEntry.mRetryCount <<
                " max: "              << mMaxRetryCount <<
                " wait time: "        << (theNow - theEntry.mTime) <<
            KFS_LOG_EOM;
            if (theStatus != kErrorRequeueRequired) {
                mStats.mOpsTimeoutCount++;
            }
            if (theEntry.mRetryCount >= mMaxRetryCount) {
                theEntry.mOpPtr->status    = theCurStatus;
                theEntry.mOpPtr->lastError = -ETIMEDOUT;
                theEntry.Done();
            } else {
                mStats.mOpsRetriedCount += theRetryIncrement;
                const OpQueueEntry theTmp = theEntry;
                theEntry.Clear();
                EnqueueSelf(theTmp.mOpPtr, theTmp.mOwnerPtr,
                    theTmp.mBufferPtr, theTmp.mRetryCount + theRetryIncrement);
            }
        }
        mQueueStack.erase(theStIt);
        if (! mPendingOpQueue.empty()) {
            EnsureConnected();
        }
    }
    int64_t GetTokenExpirationTime(
            const string& inKeyId) const
    {
        int64_t theRet = 0;
        if (! ParseTokenExpirationTime(inKeyId, theRet)) {
            const int64_t kDefaultSessionExpirationTimeSec = 10 * 24 * 60 * 60;
            KFS_LOG_STREAM_INFO << mLogPrefix <<
                "failed to parse delegation token,"
                " setting expriation time to " <<
                    kDefaultSessionExpirationTimeSec << " sec." <<
            KFS_LOG_EOM;
            return Now() + kDefaultSessionExpirationTimeSec;
        }
        return theRet;
    }
    bool ParseTokenExpirationTime(
            const string& inKeyId,
            int64_t&      outTime) const
    {
        DelegationToken theToken;
        if (inKeyId.empty() ||
                ! theToken.FromString(inKeyId.data(), inKeyId.size(), 0, 0)) {
            return false;
        }
        outTime = theToken.GetIssuedTime() + theToken.GetValidForSec();
        return true;
    }
    friend class StImplRef;
private:
    Impl(
        const Impl& inClient);
    Impl& operator=(
        const Impl& inClient);
};

KfsNetClient::KfsNetClient(
        NetManager&        inNetManager,
        string             inHost                           /* = string() */,
        int                inPort                           /* = 0 */,
        int                inMaxRetryCount                  /* = 0 */,
        int                inTimeSecBetweenRetries          /* = 10 */,
        int                inOpTimeoutSec                   /* = 5  * 60 */,
        int                inIdleTimeoutSec                 /* = 30 * 60 */,
        int64_t            inInitialSeqNum                  /* = 1 */,
        const char*        inLogPrefixPtr                   /* = 0 */,
        bool               inResetConnectionOnOpTimeoutFlag /* = true */,
        int                inMaxContentLength               /* = MAX_RPC_HEADER_LEN */,
        bool               inFailAllOpsOnOpTimeoutFlag      /* = false */,
        bool               inMaxOneOutstandingOpFlag        /* = false */,
        ClientAuthContext* inAuthContextPtr                 /* = 0 */,
        const QCThread*    inThreadPtr                      /* = 0 */)
    : mImpl(*new Impl(
        inHost,
        inPort,
        inMaxRetryCount,
        inTimeSecBetweenRetries,
        inOpTimeoutSec,
        inIdleTimeoutSec,
        kfsSeq_t(inInitialSeqNum),
        inLogPrefixPtr,
        inNetManager,
        inResetConnectionOnOpTimeoutFlag,
        inMaxContentLength,
        inFailAllOpsOnOpTimeoutFlag,
        inMaxOneOutstandingOpFlag,
        inAuthContextPtr,
        inThreadPtr
    ))
{
    mImpl.Ref();
}

    /* virtual */
KfsNetClient::~KfsNetClient()
{
    mImpl.Stop();
    mImpl.UnRef();
}

    bool
KfsNetClient::IsConnected() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.IsConnected();
}

    int64_t
KfsNetClient::GetDisconnectCount() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.GetDisconnectCount();
}

    bool
KfsNetClient::Start(
    string             inServerName,
    int                inServerPort,
    string*            inErrMsgPtr,
    bool               inRetryPendingOpsFlag,
    int                inMaxRetryCount,
    int                inTimeSecBetweenRetries,
    bool               inRetryConnectOnlyFlag,
    ClientAuthContext* inAuthContextPtr)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Start(
        inServerName,
        inServerPort,
        inErrMsgPtr,
        inRetryPendingOpsFlag,
        inMaxRetryCount,
        inTimeSecBetweenRetries,
        inRetryConnectOnlyFlag,
        inAuthContextPtr
    );
}

    bool
KfsNetClient::SetServer(
    const ServerLocation& inLocation,
    bool                  inCancelPendingOpsFlag /* = true */,
    string*               inErrMsgPtr            /* = 0 */,
    bool                  inForceConnectFlag     /* = true */)
{
    Impl::StRef theRef(mImpl);
    return mImpl.SetServer(
        inLocation, inCancelPendingOpsFlag, inErrMsgPtr, inForceConnectFlag);
}
    void
KfsNetClient::SetRpcFormat(
    RpcFormat inRpcFormat)
{
    Impl::StRef theRef(mImpl);
    return mImpl.SetRpcFormat(inRpcFormat);
}

    KfsNetClient::RpcFormat
KfsNetClient::GetRpcFormat() const
{
    return mImpl.GetRpcFormat();
}

    void
KfsNetClient::SetKey(
    const char* inKeyIdPtr,
    const char* inKeyDataPtr,
    int         inKeyDataSize)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetKey(inKeyIdPtr, inKeyDataPtr, inKeyDataSize);
}

    void
KfsNetClient::SetKey(
    const char* inKeyIdPtr,
    int         inKeyIdLen,
    const char* inKeyDataPtr,
    int         inKeyDataSize)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetKey(inKeyIdPtr, inKeyIdLen, inKeyDataPtr, inKeyDataSize);
}

    const string&
KfsNetClient::GetKey() const
{
    return mImpl.GetKey();
}

    const string&
KfsNetClient::GetKeyId() const
{
    return mImpl.GetKeyId();
}

    const string&
KfsNetClient::GetSessionKey() const
{
    return mImpl.GetSessionKey();
}

    const string&
KfsNetClient::GetSessionKeyId() const
{
    return mImpl.GetSessionKeyId();
}

    void
KfsNetClient::SetShutdownSsl(
    bool inFlag)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetShutdownSsl(inFlag);
}

    bool
KfsNetClient::IsShutdownSsl() const
{
    return mImpl.IsShutdownSsl();
}

    void
KfsNetClient::SetAuthContext(
    ClientAuthContext* inAuthContextPtr)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetAuthContext(inAuthContextPtr);
}

    ClientAuthContext*
KfsNetClient::GetAuthContext() const
{
    return mImpl.GetAuthContext();
}

    void
KfsNetClient::Stop()
{
    Impl::StRef theRef(mImpl);
    mImpl.Stop();
}

    int
KfsNetClient::GetMaxRetryCount() const
{
    return mImpl.GetMaxRetryCount();
}

    void
KfsNetClient::SetMaxRetryCount(
    int inMaxRetryCount)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetMaxRetryCount(inMaxRetryCount);
}

    int
KfsNetClient::GetIdleTimeoutSec() const
{
    return mImpl.GetIdleTimeoutSec();
}

    void
KfsNetClient::SetIdleTimeoutSec(
    int inTimeout)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetIdleTimeoutSec(inTimeout);
}

    int
KfsNetClient::GetTimeSecBetweenRetries()
{
    return mImpl.GetTimeSecBetweenRetries();
}

    void
KfsNetClient::SetTimeSecBetweenRetries(
    int inTimeSec)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetTimeSecBetweenRetries(inTimeSec);
}

    bool
KfsNetClient::IsAllDataSent() const
{
    return mImpl.IsAllDataSent();
}

    bool
KfsNetClient::IsDataReceived() const
{
    return mImpl.IsDataReceived();
}

    bool
KfsNetClient::IsDataSent() const
{
    return mImpl.IsDataSent();
}

    bool
KfsNetClient::IsRetryConnectOnly() const
{
    return mImpl.IsRetryConnectOnly();
}

    bool
KfsNetClient::WasDisconnected() const
{
    return mImpl.WasDisconnected();
}

    void
KfsNetClient::SetRetryConnectOnly(
    bool inFlag)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetRetryConnectOnly(inFlag);
}

    int
KfsNetClient::GetOpTimeoutSec() const
{
    return mImpl.GetOpTimeoutSec();
}

    void
KfsNetClient::SetOpTimeoutSec(
    int inOpTimeoutSec)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetOpTimeoutSec(inOpTimeoutSec);
}

    void
KfsNetClient::GetStats(
    Stats& outStats) const
{
    mImpl.GetStats(outStats);
}

    bool
KfsNetClient::Enqueue(
    KfsOp*    inOpPtr,
    OpOwner*  inOwnerPtr,
    IOBuffer* inBufferPtr /* = 0 */)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Enqueue(inOpPtr, inOwnerPtr, inBufferPtr);
}

    bool
KfsNetClient::Cancel(
    KfsOp*   inOpPtr,
    OpOwner* inOwnerPtr)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Cancel(inOpPtr, inOwnerPtr);
}

    bool
KfsNetClient::Cancel()
{
    Impl::StRef theRef(mImpl);
    return mImpl.Cancel();
}

    void
KfsNetClient::CancelAllWithOwner(
    OpOwner* inOwnerPtr)
{
    Impl::StRef theRef(mImpl);
    return mImpl.CancelAllWithOwner(inOwnerPtr);
}

    const ServerLocation&
KfsNetClient::GetServerLocation() const
{
    return mImpl.GetServerLocation();
}

    void
KfsNetClient::SetEventObserver(
    KfsNetClient::EventObserver* inEventObserverPtr)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetEventObserver(inEventObserverPtr);
}

    NetManager&
KfsNetClient::GetNetManager() const
{
    // This method must not change any variables including the reference count.
    // The thread logic in chunk server relies on this, when it constructs RS
    // replicator reader: the reader's constructor calls this method, but must
    // not modify in any way the meta server state machine state, including the
    // ref. count, as the state machine might be running withing a different
    // thread.
    return mImpl.GetNetManager();
}

    void
KfsNetClient::SetMaxContentLength(
    int inMax)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetMaxContentLength(inMax);
}

    void
KfsNetClient::ClearMaxOneOutstandingOpFlag()
{
    Impl::StRef theRef(mImpl);
    mImpl.ClearMaxOneOutstandingOpFlag();
}

    void
KfsNetClient::SetFailAllOpsOnOpTimeoutFlag(
    bool inFlag)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetFailAllOpsOnOpTimeoutFlag(inFlag);
}

    void
KfsNetClient::SetMaxRpcHeaderLength(
    int inMaxRpcHeaderLength)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetMaxRpcHeaderLength(inMaxRpcHeaderLength);
}

    void
KfsNetClient::SetCommonRpcHeaders(
    const string& inCommonHeaders,
    const string& inCommonShortHeaders)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetCommonRpcHeaders(inCommonHeaders, inCommonShortHeaders);
}

    void
KfsNetClient::SetNetManager(
    NetManager& inNetManager)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetNetManager(inNetManager);
}

    int
KfsNetClient::GetMaxMetaLogWriteRetryCount() const
{
    return mImpl.GetMaxMetaLogWriteRetryCount();
}

    void
KfsNetClient::SetMaxMetaLogWriteRetryCount(
    int inCount)
{
    Impl::StRef theRef(mImpl);
    mImpl.SetMaxMetaLogWriteRetryCount(inCount);
}

    bool
KfsNetClient::AddMetaServerLocation(
    const ServerLocation& inLocation,
    bool                  inAllowDuplicatesFlag)
{
    Impl::StRef theRef(mImpl);
    return mImpl.AddMetaServerLocation(inLocation, inAllowDuplicatesFlag);
}

    int
KfsNetClient::SetMetaServerLocations(
    const ServerLocation& inLocation,
    const char*           inLocationsStrPtr,
    size_t                inLocationsStrLen,
    bool                  inAllowDuplicatesFlag,
    bool                  inHexFormatFlag)
{
    Impl::StRef theRef(mImpl);
    return mImpl.SetMetaServerLocations(inLocation,
        inLocationsStrPtr, inLocationsStrLen,
        inAllowDuplicatesFlag, inHexFormatFlag);
}

    void
KfsNetClient::SetThread(
    const QCThread* inThreadPtr)
{
    // Do not change ref count, in order to allow to set different thread than
    // the current.
    mImpl.SetThread(inThreadPtr);
}

}} /* namespace cient KFS */
