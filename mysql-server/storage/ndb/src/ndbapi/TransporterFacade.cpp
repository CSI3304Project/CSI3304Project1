/*
   Copyright (c) 2003, 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>
#include <ndb_limits.h>
#include "TransporterFacade.hpp"
#include "trp_client.hpp"
#include "ClusterMgr.hpp"
#include <IPCConfig.hpp>
#include <TransporterCallback.hpp>
#include <TransporterRegistry.hpp>
#include "NdbApiSignal.hpp"
#include "NdbWaiter.hpp"
#include <NdbOut.hpp>
#include <NdbEnv.h>
#include <NdbSleep.h>
#include <NdbLockCpuUtil.h>

#include <kernel/GlobalSignalNumbers.h>
#include <mgmapi_config_parameters.h>
#include <mgmapi_configuration.hpp>
#include <NdbConfig.h>
#include <ndb_version.h>
#include <SignalLoggerManager.hpp>
#include <kernel/ndb_limits.h>
#include <signaldata/AlterTable.hpp>
#include <signaldata/SumaImpl.hpp>
#include <signaldata/AllocNodeId.hpp>
#include <signaldata/CloseComReqConf.hpp>

//#define REPORT_TRANSPORTER
//#define API_TRACE

static int numberToIndex(int number)
{
  return number - MIN_API_BLOCK_NO;
}

static int indexToNumber(int index)
{
  return index + MIN_API_BLOCK_NO;
}

#if defined DEBUG_TRANSPORTER
#define TRP_DEBUG(t) ndbout << __FILE__ << ":" << __LINE__ << ":" << t << endl;
#else
#define TRP_DEBUG(t)
#endif

#define DBG_POLL 0
#define dbg(x,y) //if (DBG_POLL) printf("%llu : " x "\n", NdbTick_CurrentNanosecond() / 1000, y)

/*****************************************************************************
 * Call back functions
 *****************************************************************************/
void
TransporterFacade::reportError(NodeId nodeId,
                               TransporterError errorCode, const char *info)
{
#ifdef REPORT_TRANSPORTER
  ndbout_c("REPORT_TRANSP: reportError (nodeId=%d, errorCode=%d) %s", 
	   (int)nodeId, (int)errorCode, info ? info : "");
#endif
  if(errorCode & TE_DO_DISCONNECT) {
    ndbout_c("reportError (%d, %d) %s", (int)nodeId, (int)errorCode,
	     info ? info : "");
    doDisconnect(nodeId);
  }
}

/**
 * Report average send length in bytes (4096 last sends)
 */
void
TransporterFacade::reportSendLen(NodeId nodeId, Uint32 count, Uint64 bytes)
{
#ifdef REPORT_TRANSPORTER
  ndbout_c("REPORT_TRANSP: reportSendLen (nodeId=%d, bytes/count=%d)", 
	   (int)nodeId, (Uint32)(bytes/count));
#endif
  (void)nodeId;
  (void)count;
  (void)bytes;
}

/** 
 * Report average receive length in bytes (4096 last receives)
 */
void
TransporterFacade::reportReceiveLen(NodeId nodeId, Uint32 count, Uint64 bytes)
{
#ifdef REPORT_TRANSPORTER
  ndbout_c("REPORT_TRANSP: reportReceiveLen (nodeId=%d, bytes/count=%d)", 
	   (int)nodeId, (Uint32)(bytes/count));
#endif
  (void)nodeId;
  (void)count;
  (void)bytes;
}

/**
 * Report connection established
 */
void
TransporterFacade::reportConnect(NodeId nodeId)
{
#ifdef REPORT_TRANSPORTER
  ndbout_c("REPORT_TRANSP: API reportConnect (nodeId=%d)", (int)nodeId);
#endif
  reportConnected(nodeId);
}

/**
 * Report connection broken
 */
void
TransporterFacade::reportDisconnect(NodeId nodeId, Uint32 error){
#ifdef REPORT_TRANSPORTER
  ndbout_c("REPORT_TRANSP: API reportDisconnect (nodeId=%d)", (int)nodeId);
#endif
  reportDisconnected(nodeId);
}

void
TransporterFacade::transporter_recv_from(NodeId nodeId)
{
  hb_received(nodeId);
}

/****************************************************************************
 * 
 *****************************************************************************/

/**
 * Report connection broken
 */
int
TransporterFacade::checkJobBuffer()
{
  return 0;
}

#ifdef API_TRACE
static const char * API_SIGNAL_LOG = "API_SIGNAL_LOG";
static const char * apiSignalLog   = 0;
static SignalLoggerManager signalLogger;
static
inline
bool
setSignalLog(){
  signalLogger.flushSignalLog();

  const char * tmp = NdbEnv_GetEnv(API_SIGNAL_LOG, (char *)0, 0);
  if(tmp != 0 && apiSignalLog != 0 && strcmp(tmp,apiSignalLog) == 0){
    return true;
  } else if(tmp == 0 && apiSignalLog == 0){
    return false;
  } else if(tmp == 0 && apiSignalLog != 0){
    signalLogger.setOutputStream(0);
    apiSignalLog = tmp;
    return false;
  } else if(tmp !=0){
    if (strcmp(tmp, "-") == 0)
        signalLogger.setOutputStream(stdout);
#ifndef DBUG_OFF
    else if (strcmp(tmp, "+") == 0)
        signalLogger.setOutputStream(DBUG_FILE);
#endif
    else
        signalLogger.setOutputStream(fopen(tmp, "w"));
    apiSignalLog = tmp;
    return true;
  }
  return false;
}
inline
bool
TRACE_GSN(Uint32 gsn)
{
  switch(gsn){
#ifndef TRACE_APIREGREQ
  case GSN_API_REGREQ:
  case GSN_API_REGCONF:
    return false;
#endif
#if 1
  case GSN_SUB_GCP_COMPLETE_REP:
  case GSN_SUB_GCP_COMPLETE_ACK:
    return false;
#endif
  default:
    return true;
  }
}
#endif

/**
 * The execute function : Handle received signal
 */
bool
TransporterFacade::deliver_signal(SignalHeader * const header,
                                  Uint8 prio, Uint32 * const theData,
                                  LinearSectionPtr ptr[3])
{
  Uint32 tRecBlockNo = header->theReceiversBlockNumber;

#ifdef API_TRACE
  if(setSignalLog() && TRACE_GSN(header->theVerId_signalNumber)){
    signalLogger.executeSignal(* header, 
                               prio,
                               theData,
                               ownId(),
                               ptr, header->m_noOfSections);
    signalLogger.flushSignalLog();
  }
#endif  

  if (tRecBlockNo >= MIN_API_BLOCK_NO)
  {
    trp_client * clnt = m_threads.get(tRecBlockNo);
    if (clnt != 0)
    {
      m_poll_owner->m_poll.lock_client(clnt);
      /**
       * Handle received signal immediately to avoid any unnecessary
       * copying of data, allocation of memory and other things. Copying
       * of data could be interesting to support several priority levels
       * and to support a special memory structure when executing the
       * signals. Neither of those are interesting when receiving data
       * in the NDBAPI. The NDBAPI will thus read signal data directly as
       * it was written by the sender (SCI sender is other node, Shared
       * memory sender is other process and TCP/IP sender is the OS that
       * writes the TCP/IP message into a message buffer).
       */
      NdbApiSignal tmpSignal(*header);
      NdbApiSignal * tSignal = &tmpSignal;
      tSignal->setDataPtr(theData);
      clnt->trp_deliver_signal(tSignal, ptr);
    }
    else
    {
      handleMissingClnt(header, theData);
    }
  }
  else if (tRecBlockNo == API_PACKED)
  {
    /**
     * Block number == 2047 is used to signal a signal that consists of
     * multiple instances of the same signal. This is an effort to
     * package the signals so as to avoid unnecessary communication
     * overhead since TCP/IP has a great performance impact.
     */
    Uint32 Tlength = header->theLength;
    Uint32 Tsent = 0;
    /**
     * Since it contains at least two data packets we will first
     * copy the signal data to safe place.
     */
    while (Tsent < Tlength) {
      Uint32 Theader = theData[Tsent];
      Tsent++;
      Uint32 TpacketLen = (Theader & 0x1F) + 3;
      tRecBlockNo = Theader >> 16;
      if (TpacketLen <= 25)
      {
        if ((TpacketLen + Tsent) <= Tlength)
        {
          /**
           * Set the data length of the signal and the receivers block
           * reference and then call the API.
           */
          header->theLength = TpacketLen;
          header->theReceiversBlockNumber = tRecBlockNo;
          Uint32* tDataPtr = &theData[Tsent];
          Tsent += TpacketLen;
          if (tRecBlockNo >= MIN_API_BLOCK_NO)
          {
            trp_client * clnt = m_threads.get(tRecBlockNo);
            if(clnt != 0)
            {
              NdbApiSignal tmpSignal(*header);
              NdbApiSignal * tSignal = &tmpSignal;
              tSignal->setDataPtr(tDataPtr);
              m_poll_owner->m_poll.lock_client(clnt);
              clnt->trp_deliver_signal(tSignal, 0);
            }
            else
            {
              handleMissingClnt(header, tDataPtr);
            }
          }
        }
      }
    }
  }
  else if (tRecBlockNo >= MIN_API_FIXED_BLOCK_NO &&
           tRecBlockNo <= MAX_API_FIXED_BLOCK_NO) 
  {
    Uint32 dynamic= m_fixed2dynamic[tRecBlockNo - MIN_API_FIXED_BLOCK_NO];
    trp_client * clnt = m_threads.get(dynamic);
    if (clnt != 0)
    {
      NdbApiSignal tmpSignal(*header);
      NdbApiSignal * tSignal = &tmpSignal;
      tSignal->setDataPtr(theData);
      m_poll_owner->m_poll.lock_client(clnt);
      clnt->trp_deliver_signal(tSignal, ptr);
    }
    else
    {
      handleMissingClnt(header, theData);
    }
  }
  else
  {
    // Ignore all other block numbers.
    if(header->theVerId_signalNumber != GSN_API_REGREQ)
    {
      TRP_DEBUG( "TransporterFacade received signal to unknown block no." );
      ndbout << "BLOCK NO: "  << tRecBlockNo << " sig " 
             << header->theVerId_signalNumber  << endl;
      ndbout << *header << "-- Signal Data --" << endl;
      ndbout.hexdump(theData, MAX(header->theLength, 25)) << flush;
      abort();
    }
  }

  /**
   * API_PACKED contains a number of messages,
   * We need to have space for all of them, a
   * maximum of six signals can be carried in
   * one packed signal to the NDB API.
   */
  const Uint32 MAX_MESSAGES_IN_LOCKED_CLIENTS =
    m_poll_owner->m_poll.m_lock_array_size - 6;
   return m_poll_owner->m_poll.m_locked_cnt >= MAX_MESSAGES_IN_LOCKED_CLIENTS;
}

#include <signaldata/TcKeyConf.hpp>
#include <signaldata/TcCommit.hpp>
#include <signaldata/TcKeyFailConf.hpp>

void
TransporterFacade::handleMissingClnt(const SignalHeader * header,
                                     const Uint32 * theData)
{
  Uint32 gsn = header->theVerId_signalNumber;
  Uint32 transId[2];
  if (gsn == GSN_TCKEYCONF || gsn == GSN_TCINDXCONF)
  {
    const TcKeyConf * conf = CAST_CONSTPTR(TcKeyConf, theData);
    if (TcKeyConf::getMarkerFlag(conf->confInfo) == false)
    {
      return;
    }
    transId[0] = conf->transId1;
    transId[1] = conf->transId2;
  }
  else if (gsn == GSN_TC_COMMITCONF)
  {
    const TcCommitConf * conf = CAST_CONSTPTR(TcCommitConf, theData);
    if ((conf->apiConnectPtr & 1) == 0)
    {
      return;
    }
    transId[0] = conf->transId1;
    transId[1] = conf->transId2;
  }
  else if (gsn == GSN_TCKEY_FAILCONF)
  {
    const TcKeyFailConf * conf = CAST_CONSTPTR(TcKeyFailConf, theData);
    if ((conf->apiConnectPtr & 1) == 0)
    {
      return;
    }
    transId[0] = conf->transId1;
    transId[1] = conf->transId2;
  }
  else
  {
    return;
  }

  // ndbout_c("KESO KESO KESO: sending commit ack marker 0x%.8x 0x%.8x (gsn: %u)",
  //         transId[0], transId[1], gsn);

  Uint32 ownBlockNo = header->theReceiversBlockNumber;
  Uint32 aTCRef = header->theSendersBlockRef;

  NdbApiSignal tSignal(numberToRef(ownBlockNo, ownId()));
  tSignal.theReceiversBlockNumber = refToBlock(aTCRef);
  tSignal.theVerId_signalNumber   = GSN_TC_COMMIT_ACK;
  tSignal.theLength               = 2;

  Uint32 * dataPtr = tSignal.getDataPtrSend();
  dataPtr[0] = transId[0];
  dataPtr[1] = transId[1];

  m_poll_owner->safe_sendSignal(&tSignal, refToNode(aTCRef));
}

// These symbols are needed, but not used in the API
void 
SignalLoggerManager::printSegmentedSection(FILE *, const SignalHeader &,
					   const SegmentedSectionPtr ptr[3],
					   unsigned i){
  abort();
}

void 
copy(Uint32 * & insertPtr, 
     class SectionSegmentPool & thePool, const SegmentedSectionPtr & _ptr){
  abort();
}

/**
 * Note that this function needs no locking since it is
 * only called from the constructor of Ndb (the NdbObject)
 * 
 * Which is protected by a mutex
 */

int
TransporterFacade::start_instance(NodeId nodeId,
                                  const ndb_mgm_configuration* conf)
{
  assert(theOwnId == 0);
  theOwnId = nodeId;

#if defined SIGPIPE && !defined _WIN32
  (void)signal(SIGPIPE, SIG_IGN);
#endif

  theTransporterRegistry = new TransporterRegistry(this, this, false);
  if (theTransporterRegistry == NULL)
    return -1;

  if (!theTransporterRegistry->init(nodeId))
    return -1;

  if (theClusterMgr == NULL)
    theClusterMgr = new ClusterMgr(*this);

  if (theClusterMgr == NULL)
    return -1;

  if (!configure(nodeId, conf))
    return -1;

  if (!theTransporterRegistry->start_service(m_socket_server))
    return -1;

  theReceiveThread = NdbThread_Create(runReceiveResponse_C,
                                      (void**)this,
                                      0, // Use default stack size
                                      "ndb_receive",
                                      NDB_THREAD_PRIO_LOW);

  theSendThread = NdbThread_Create(runSendRequest_C,
                                   (void**)this,
                                   0, // Use default stack size
                                   "ndb_send",
                                   NDB_THREAD_PRIO_LOW);

  theClusterMgr->startThread();

  return 0;
}

void
TransporterFacade::stop_instance(){
  DBUG_ENTER("TransporterFacade::stop_instance");

  // Stop the send and receive threads
  void *status;
  theStopReceive = 1;
  if (theReceiveThread) {
    NdbThread_WaitFor(theReceiveThread, &status);
    NdbThread_Destroy(&theReceiveThread);
  }
  theStopSend = 1;
  if (theSendThread) {
    NdbThread_WaitFor(theSendThread, &status);
    NdbThread_Destroy(&theSendThread);
  }

  // Stop clustmgr last as (currently) recv thread accesses clusterMgr
  if (theClusterMgr)
  {
    theClusterMgr->doStop();
  }

  DBUG_VOID_RETURN;
}

void TransporterFacade::setSendThreadInterval(Uint32 ms)
{
  if(ms > 0 && ms <= 10) 
  { 
    sendThreadWaitMillisec = ms;
  }
}

Uint32 TransporterFacade::getSendThreadInterval(void)
{
  return sendThreadWaitMillisec;
}

extern "C" 
void* 
runSendRequest_C(void * me)
{
  ((TransporterFacade*) me)->threadMainSend();
  return 0;
}

static inline
void
link_buffer(TFBuffer* dst, const TFBuffer * src)
{
  assert(dst);
  assert(src);
  assert(src->m_head);
  assert(src->m_tail);
  TFBufferGuard g0(* dst);
  TFBufferGuard g1(* src);
  if (dst->m_head == 0)
  {
    dst->m_head = src->m_head;
  }
  else
  {
    dst->m_tail->m_next = src->m_head;
  }
  dst->m_tail = src->m_tail;
  dst->m_bytes_in_buffer += src->m_bytes_in_buffer;
}

static const Uint32 SEND_THREAD_NO = 0;

void
TransporterFacade::wakeup_send_thread(void)
{
  Guard g(m_send_thread_mutex);
  if (m_send_thread_nodes.get(SEND_THREAD_NO) == false)
  {
    NdbCondition_Signal(m_send_thread_cond);
  }
  m_send_thread_nodes.set(SEND_THREAD_NO);
}

void TransporterFacade::threadMainSend(void)
{
  while (theSendThread == NULL)
  {
    /* Wait until theSendThread have been set */
    NdbSleep_MilliSleep(10);
  }
  theTransporterRegistry->startSending();
  if (theTransporterRegistry->start_clients() == 0){
    ndbout_c("Unable to start theTransporterRegistry->start_clients");
    exit(0);
  }

  m_socket_server.startServer();

  while(!theStopSend)
  {
    NdbMutex_Lock(m_send_thread_mutex);
    if (m_send_thread_nodes.get(SEND_THREAD_NO) == false)
    {
      NdbCondition_WaitTimeout(m_send_thread_cond,
                               m_send_thread_mutex,
                               sendThreadWaitMillisec);
    }
    m_send_thread_nodes.clear(SEND_THREAD_NO);
    NdbMutex_Unlock(m_send_thread_mutex);
    bool all_empty = true;
    do
    {
      all_empty = true;
      for (Uint32 i = 0; i<MAX_NODES; i++)
      {
        struct TFSendBuffer * b = m_send_buffers + i;
        if (!b->m_node_active)
          continue;
        NdbMutex_Lock(&b->m_mutex);
        if (b->m_sending)
        {
          /**
           * sender does stuff when clearing m_sending
           */
        }
        else if (b->m_buffer.m_bytes_in_buffer > 0 ||
                 b->m_out_buffer.m_bytes_in_buffer > 0)
        {
          /**
           * Copy all data from m_buffer to m_out_buffer
           */
          TFBuffer copy = b->m_buffer;
          memset(&b->m_buffer, 0, sizeof(b->m_buffer));
          b->m_sending = true;
          NdbMutex_Unlock(&b->m_mutex);
          if (copy.m_bytes_in_buffer > 0)
          {
            link_buffer(&b->m_out_buffer, &copy);
          }
          theTransporterRegistry->performSend(i);
          NdbMutex_Lock(&b->m_mutex);
          b->m_sending = false;
          if (b->m_buffer.m_bytes_in_buffer > 0 ||
              b->m_out_buffer.m_bytes_in_buffer > 0)
          {
            all_empty = false;
          }
        }
        NdbMutex_Unlock(&b->m_mutex);
      }
    } while (!theStopSend && all_empty == false);

  }
  theTransporterRegistry->stopSending();

  m_socket_server.stopServer();
  m_socket_server.stopSessions(true);

  theTransporterRegistry->stop_clients();
}

extern "C" 
void* 
runReceiveResponse_C(void * me)
{
  ((TransporterFacade*) me)->threadMainReceive();
  return 0;
}

ReceiveThreadClient::ReceiveThreadClient(TransporterFacade * facade)
{
  DBUG_ENTER("ReceiveThreadClient::ReceiveThreadClient");
  Uint32 ret = this->open(facade, -1, true);
  if (unlikely(ret == 0))
  {
    ndbout_c("Failed to register receive thread, ret = %d", ret);
    abort();
  }
  DBUG_VOID_RETURN;
}

ReceiveThreadClient::~ReceiveThreadClient()
{
  DBUG_ENTER("ReceiveThreadClient::~ReceiveThreadClient");
  this->close();
  DBUG_VOID_RETURN;
}

void
ReceiveThreadClient::trp_deliver_signal(const NdbApiSignal *signal,
                                        const LinearSectionPtr ptr[3])
{
  DBUG_ENTER("ReceiveThreadClient::trp_deliver_signal");
  switch (signal->theVerId_signalNumber)
  {
    case GSN_API_REGCONF:
    case GSN_CONNECT_REP:
    case GSN_NODE_FAILREP:
    case GSN_NF_COMPLETEREP:
    case GSN_TAKE_OVERTCCONF:
    case GSN_ALLOC_NODEID_CONF:
    case GSN_SUB_GCP_COMPLETE_REP:
      break;
    case GSN_CLOSE_COMREQ:
    {
      m_facade->perform_close_clnt(this);
      break;
    }
    default:
    {
      ndbout_c("Receive thread block should not receive signals, gsn: %d",
               signal->theVerId_signalNumber);
      abort();
    }
  }
  DBUG_VOID_RETURN;
}

void
TransporterFacade::checkClusterMgr(NDB_TICKS & lastTime)
{
  lastTime = NdbTick_getCurrentTicks();
  theClusterMgr->lock();
  theTransporterRegistry->update_connections();
  theClusterMgr->flush_send_buffers();
  theClusterMgr->unlock();
}

bool
TransporterFacade::become_poll_owner(trp_client* clnt,
                                     NDB_TICKS currTime)
{
  bool poll_owner = false;
  lock_poll_mutex();
  if (m_poll_owner == NULL)
  {
    poll_owner = true;
    m_num_active_clients = 0;
    m_receive_activation_time = currTime;
    m_poll_owner = clnt;
  }
  unlock_poll_mutex();
  if (poll_owner)
  {
    return true;
  }
  return false;
}

int
TransporterFacade::unset_recv_thread_cpu(Uint32 recv_thread_id)
{
  if (recv_thread_id != 0)
  {
    return -1;
  }
  unlock_recv_thread_cpu();
  recv_thread_cpu_id = NO_RECV_THREAD_CPU_ID;
  return 0;
}

int
TransporterFacade::set_recv_thread_cpu(Uint16 *cpuid_array,
                                       Uint32 array_len,
                                       Uint32 recv_thread_id)
{
  if (array_len > 1 || array_len == 0)
  {
    return -1;
  }
  if (recv_thread_id != 0)
  {
    return -1;
  }
  recv_thread_cpu_id = cpuid_array[0];
  if (theTransporterRegistry)
  {
    /* Receiver thread already started, lock cpu now */
    lock_recv_thread_cpu();
  }
  return 0;
}

int
TransporterFacade::set_recv_thread_activation_threshold(Uint32 threshold)
{
  if (threshold >= 16)
  {
    threshold = 256;
  }
  min_active_clients_recv_thread = threshold;
  return 0;
}

void
TransporterFacade::unlock_recv_thread_cpu()
{
  if (theReceiveThread)
    Ndb_UnlockCPU(theReceiveThread);
}

void
TransporterFacade::lock_recv_thread_cpu()
{
  Uint32 cpu_id = recv_thread_cpu_id;
  if (cpu_id != NO_RECV_THREAD_CPU_ID && theReceiveThread)
  {
    Ndb_LockCPU(theReceiveThread, cpu_id);
  }
}

int
TransporterFacade::get_recv_thread_activation_threshold() const
{
  return min_active_clients_recv_thread;
}

static const int DEFAULT_MIN_ACTIVE_CLIENTS_RECV_THREAD = 8;
/*
  The receiver thread is changed to only wake up once every 10 milliseconds
  to poll. It will first check that nobody owns the poll "right" before
  polling. This means that methods using the receiveResponse and
  sendRecSignal will have a slightly longer response time if they are
  executed without any parallel key lookups. Currently also scans are
  affected but this is to be fixed.
*/
void TransporterFacade::threadMainReceive(void)
{
  bool poll_owner = false;
  bool check_cluster_mgr;
  NDB_TICKS currTime = NdbTick_getCurrentTicks();
  NDB_TICKS lastTime = currTime;

  while (theReceiveThread == NULL)
  {
    /* Wait until theReceiveThread have been set */
    NdbSleep_MilliSleep(10);
  }
  theTransporterRegistry->startReceiving();
#ifdef NDB_SHM_TRANSPORTER
  NdbThread_set_shm_sigmask(TRUE);
#endif
  ReceiveThreadClient *recv_client = new ReceiveThreadClient(this);
  lock_recv_thread_cpu();
  while(!theStopReceive)
  {
    currTime = NdbTick_getCurrentTicks();
    Uint64 elapsed = NdbTick_Elapsed(lastTime,currTime).milliSec();
    check_cluster_mgr = false;
    if (elapsed > 100)
      check_cluster_mgr = true; /* 100 milliseconds have passed */
    if (!poll_owner)
    {
      /*
         We only take the step to become poll owner in receive thread if
         we are sufficiently active, at least e.g. 16 threads active.
         We check this condition without mutex, there is no issue with
         what we select here, both paths will work.
      */
      if (m_num_active_clients > min_active_clients_recv_thread)
      {
        poll_owner = become_poll_owner(recv_client, currTime);
      }
      else
      {
        NdbSleep_MilliSleep(100);
      }
    }
    if (poll_owner)
    {
      bool stay_poll_owner = !check_cluster_mgr;
      elapsed = NdbTick_Elapsed(m_receive_activation_time,currTime).milliSec();
      if (elapsed > 1000)
      {
        /* Reset timer for next activation check time */
        m_receive_activation_time = currTime;
        lock_poll_mutex();
        if (m_num_active_clients < (min_active_clients_recv_thread / 2))
        {
          /* Go back to not have an active receive thread */
          stay_poll_owner = false;
        }
        m_num_active_clients = 0; /* Reset active clients for next timeslot */
        unlock_poll_mutex();
      }
      recv_client->start_poll();
      do_poll(recv_client, 10, true, stay_poll_owner);
      recv_client->complete_poll();
      poll_owner = stay_poll_owner;
    }
    if (check_cluster_mgr)
    {
      /*
        ensure that this thread is not poll owner before calling
        checkClusterMgr to avoid ending up in a deadlock when
        acquiring locks on cluster manager mutexes.
      */
      assert(!poll_owner);
      checkClusterMgr(lastTime);
    }
  }

  if (poll_owner)
  {
    /*
      Ensure to release poll ownership before proceeding to delete the
      transporter client and thus close it. That code expects not to be
      called when being the poll owner.
    */
    recv_client->start_poll();
    do_poll(recv_client, 0, true, false);
    recv_client->complete_poll();
  }
  delete recv_client;
  theTransporterRegistry->stopReceiving();
}

/*
  This method is called by worker thread that owns the poll "rights".
  It waits for events and if something arrives it takes care of it
  and returns to caller. It will quickly come back here if not all
  data was received for the worker thread.
*/
void
TransporterFacade::external_poll(Uint32 wait_time)
{
#ifdef NDB_SHM_TRANSPORTER
  /*
    If shared memory transporters are used we need to set our sigmask
    such that we wake up also on interrupts on the shared memory
    interrupt signal.
  */
  NdbThread_set_shm_sigmask(FALSE);
#endif

  const int res = theTransporterRegistry->pollReceive(wait_time);

#ifdef NDB_SHM_TRANSPORTER
  NdbThread_set_shm_sigmask(TRUE);
#endif

  if (res > 0)
  {
    theTransporterRegistry->performReceive();
  }
}

TransporterFacade::TransporterFacade(GlobalDictCache *cache) :
  min_active_clients_recv_thread(DEFAULT_MIN_ACTIVE_CLIENTS_RECV_THREAD),
  recv_thread_cpu_id(NO_RECV_THREAD_CPU_ID),
  m_poll_owner(NULL),
  m_poll_queue_head(NULL),
  m_poll_queue_tail(NULL),
  m_num_active_clients(0),
  theTransporterRegistry(0),
  theOwnId(0),
  theStartNodeId(1),
  theClusterMgr(NULL),
  checkCounter(4),
  currentSendLimit(1),
  dozer(NULL),
  theStopReceive(0),
  theStopSend(0),
  sendThreadWaitMillisec(10),
  theSendThread(NULL),
  theReceiveThread(NULL),
  m_fragmented_signal_id(0),
  m_globalDictCache(cache),
  m_send_buffer("sendbufferpool")
{
  DBUG_ENTER("TransporterFacade::TransporterFacade");
  thePollMutex = NdbMutex_CreateWithName("PollMutex");
  sendPerformedLastInterval = 0;
  m_open_close_mutex = NdbMutex_Create();
  for (Uint32 i = 0; i < NDB_ARRAY_SIZE(m_send_buffers); i++)
  {
    BaseString n;
    n.assfmt("sendbuffer:%u", i);
    NdbMutex_InitWithName(&m_send_buffers[i].m_mutex, n.c_str());
  }

  m_send_thread_cond = NdbCondition_Create();
  m_send_thread_mutex = NdbMutex_CreateWithName("SendThreadMutex");

  for (int i = 0; i < NO_API_FIXED_BLOCKS; i++)
    m_fixed2dynamic[i]= RNIL;

#ifdef API_TRACE
  apiSignalLog = 0;
#endif

  theClusterMgr = new ClusterMgr(*this);
  DBUG_VOID_RETURN;
}


/* Return true if node with "nodeId" is a MGM node */
static bool is_mgmd(Uint32 nodeId,
                    const ndb_mgm_configuration * conf)
{
  ndb_mgm_configuration_iterator iter(*conf, CFG_SECTION_NODE);
  if (iter.find(CFG_NODE_ID, nodeId))
    abort();
  Uint32 type;
  if(iter.get(CFG_TYPE_OF_SECTION, &type))
    abort();

  return (type == NODE_TYPE_MGM);
}


bool
TransporterFacade::do_connect_mgm(NodeId nodeId,
                                  const ndb_mgm_configuration* conf)
{
  // Allow other MGM nodes to connect
  DBUG_ENTER("TransporterFacade::do_connect_mgm");
  ndb_mgm_configuration_iterator iter(*conf, CFG_SECTION_CONNECTION);
  for(iter.first(); iter.valid(); iter.next())
  {
    Uint32 nodeId1, nodeId2;
    if (iter.get(CFG_CONNECTION_NODE_1, &nodeId1) ||
        iter.get(CFG_CONNECTION_NODE_2, &nodeId2))
      DBUG_RETURN(false);

    // Skip connections where this node is not involved
    if (nodeId1 != nodeId && nodeId2 != nodeId)
      continue;

    // If both sides are MGM, open connection
    if(is_mgmd(nodeId1, conf) && is_mgmd(nodeId2, conf))
    {
      Uint32 remoteNodeId = (nodeId == nodeId1 ? nodeId2 : nodeId1);
      DBUG_PRINT("info", ("opening connection to node %d", remoteNodeId));
      doConnect(remoteNodeId);
    }
  }

  DBUG_RETURN(true);
}

void
TransporterFacade::set_up_node_active_in_send_buffers(Uint32 nodeId,
                                   const ndb_mgm_configuration &conf)
{
  DBUG_ENTER("TransporterFacade::set_up_node_active_in_send_buffers");
  ndb_mgm_configuration_iterator iter(conf, CFG_SECTION_CONNECTION);
  Uint32 nodeId1, nodeId2, remoteNodeId;
  struct TFSendBuffer *b;

  /* Need to also communicate with myself, not found in config */
  b = m_send_buffers + nodeId;
  b->m_node_active = true;

  for (iter.first(); iter.valid(); iter.next())
  {
    if (iter.get(CFG_CONNECTION_NODE_1, &nodeId1)) continue;
    if (iter.get(CFG_CONNECTION_NODE_2, &nodeId2)) continue;
    if (nodeId1 != nodeId && nodeId2 != nodeId) continue;
    remoteNodeId = (nodeId == nodeId1 ? nodeId2 : nodeId1);
    b = m_send_buffers + remoteNodeId;
    b->m_node_active = true;
  }
  DBUG_VOID_RETURN;
}

bool
TransporterFacade::configure(NodeId nodeId,
                             const ndb_mgm_configuration* conf)
{
  DBUG_ENTER("TransporterFacade::configure");

  assert(theOwnId == nodeId);
  assert(theTransporterRegistry);
  assert(theClusterMgr);

  /* Set up active communication with all configured nodes */
  set_up_node_active_in_send_buffers(nodeId, *conf);

  // Configure transporters
  if (!IPCConfig::configureTransporters(nodeId,
                                        * conf,
                                        * theTransporterRegistry,
                                        true))
    DBUG_RETURN(false);

  // Configure cluster manager
  theClusterMgr->configure(nodeId, conf);

  ndb_mgm_configuration_iterator iter(* conf, CFG_SECTION_NODE);
  if(iter.find(CFG_NODE_ID, nodeId))
    DBUG_RETURN(false);

  // Configure send buffers
  if (!m_send_buffer.inited())
  {
    Uint32 total_send_buffer = 0;
    Uint64 total_send_buffer64;
    size_t total_send_buffer_size_t;
    iter.get(CFG_TOTAL_SEND_BUFFER_MEMORY, &total_send_buffer);

    total_send_buffer64 = total_send_buffer;
    if (total_send_buffer64 == 0)
    {
      total_send_buffer64 = theTransporterRegistry->get_total_max_send_buffer();
    }

    Uint64 extra_send_buffer = 0;
    iter.get(CFG_EXTRA_SEND_BUFFER_MEMORY, &extra_send_buffer);

    total_send_buffer64 += extra_send_buffer;
#if SIZEOF_CHARP == 4
    /* init method can only handle 32-bit sizes on 32-bit platforms */
    if (total_send_buffer64 > 0xFFFFFFFF)
    {
      total_send_buffer64 = 0xFFFFFFFF;
    }
#endif
    total_send_buffer_size_t = (size_t)total_send_buffer64;
    if (!m_send_buffer.init(total_send_buffer_size_t))
    {
      ndbout << "Unable to allocate "
             << total_send_buffer_size_t
             << " bytes of memory for send buffers!!" << endl;
      return false;
    }
  }

  Uint32 auto_reconnect=1;
  iter.get(CFG_AUTO_RECONNECT, &auto_reconnect);

  const char * priospec = 0;
  if (iter.get(CFG_HB_THREAD_PRIO, &priospec) == 0)
  {
    NdbThread_SetHighPrioProperties(priospec);
  }

  /**
   * Keep value it set before connect (overriding config)
   */
  if (theClusterMgr->m_auto_reconnect == -1)
  {
    theClusterMgr->m_auto_reconnect = auto_reconnect;
  }
  
#ifdef API_TRACE
  signalLogger.logOn(true, 0, SignalLoggerManager::LogInOut);
#endif

  // Open connection between MGM servers
  if (!do_connect_mgm(nodeId, conf))
    DBUG_RETURN(false);

  /**
   * Also setup Loopback Transporter
   */
  doConnect(nodeId);

  DBUG_RETURN(true);
}

void
TransporterFacade::for_each(trp_client* sender,
                            const NdbApiSignal* aSignal, 
                            const LinearSectionPtr ptr[3])
{
  /*
    Allow up to 16 threads to receive signals here before we start
    waking them up.
  */
  trp_client * woken[16];
  Uint32 cnt_woken = 0;
  Uint32 sz = m_threads.m_statusNext.size();
  for (Uint32 i = 0; i < sz ; i ++) 
  {
    trp_client * clnt = m_threads.m_objectExecute[i];
    if (clnt != 0 && clnt != sender)
    {
      bool res = m_poll_owner->m_poll.check_if_locked(clnt, (Uint32)0);
      if (res)
      {
        clnt->trp_deliver_signal(aSignal, ptr);
      }
      else
      {
        NdbMutex_Lock(clnt->m_mutex);
        int save = clnt->m_poll.m_waiting;
        clnt->trp_deliver_signal(aSignal, ptr);
        if (save != clnt->m_poll.m_waiting &&
            clnt->m_poll.m_waiting == trp_client::PollQueue::PQ_WOKEN)
        {
          woken[cnt_woken++] = clnt;
          if (cnt_woken == NDB_ARRAY_SIZE(woken))
          {
            lock_poll_mutex();
            remove_from_poll_queue(woken, cnt_woken);
            unlock_poll_mutex();
            unlock_and_signal(woken, cnt_woken);
            cnt_woken = 0;
          }
        }
        else
        {
          NdbMutex_Unlock(clnt->m_mutex);
        }
      }
    }
  }

  if (cnt_woken != 0)
  {
    lock_poll_mutex();
    remove_from_poll_queue(woken, cnt_woken);
    unlock_poll_mutex();
    unlock_and_signal(woken, cnt_woken);
  }
}

void
TransporterFacade::connected()
{
  DBUG_ENTER("TransporterFacade::connected");
  NdbApiSignal signal(numberToRef(API_CLUSTERMGR, theOwnId));
  signal.theVerId_signalNumber = GSN_ALLOC_NODEID_CONF;
  signal.theReceiversBlockNumber = 0;
  signal.theTrace  = 0;
  signal.theLength = AllocNodeIdConf::SignalLength;

  AllocNodeIdConf * rep = CAST_PTR(AllocNodeIdConf, signal.getDataPtrSend());
  rep->senderRef = 0;
  rep->senderData = 0;
  rep->nodeId = theOwnId;
  rep->secret_lo = 0;
  rep->secret_hi = 0;

  Uint32 sz = m_threads.m_statusNext.size();
  for (Uint32 i = 0; i < sz ; i ++)
  {
    trp_client * clnt = m_threads.m_objectExecute[i];
    if (clnt != 0)
    {
      clnt->trp_deliver_signal(&signal, 0);
    }
  }
  DBUG_VOID_RETURN;
}

void
TransporterFacade::perform_close_clnt(trp_client* clnt)
{
  m_threads.close(clnt->m_blockNo);
  clnt->wakeup();
}

int 
TransporterFacade::close_clnt(trp_client* clnt)
{
  bool first = true;
  NdbApiSignal signal(numberToRef(clnt->m_blockNo, theOwnId));
  signal.theVerId_signalNumber = GSN_CLOSE_COMREQ;
  signal.theTrace = 0;
  signal.theLength = 1;
  CloseComReqConf * req = CAST_PTR(CloseComReqConf, signal.getDataPtrSend());
  req->xxxBlockRef = numberToRef(clnt->m_blockNo, theOwnId);

  if (clnt)
  {
    Guard g(m_open_close_mutex);
    signal.theReceiversBlockNumber = clnt->m_blockNo;
    signal.theData[0] = clnt->m_blockNo;
    if (DBG_POLL) ndbout_c("close(%p)", clnt);
    if (m_threads.get(clnt->m_blockNo) != clnt)
    {
      abort();
    }

    /**
     * We close a client through the following procedure
     * 1) Ensure we close something which is open
     * 2) Send a signal to ourselves, this signal will be executed
     *    by the poll owner. When this signal is executed we're
     *    in the correct thread to write NULL into the mapping
     *    array.
     * 3) When this thread receives the signal sent to ourselves it
     *    it will call close (perform_close_clnt) on the client
     *    mapping.
     * 4) We will wait on a condition in this thread for the poll
     *    owner to set this entry to NULL.
     */
    if (!theTransporterRegistry)
    {
      /*
        We haven't even setup transporter registry, so no need to
        send signal to poll waiter to close.
      */
      m_threads.close(clnt->m_blockNo);
      return 0;
    }
    bool not_finished;
    do
    {
      clnt->start_poll();
      if (first)
      {
        clnt->raw_sendSignal(&signal, theOwnId);
        clnt->do_forceSend(1);
        first = false;
      }
      clnt->do_poll(0);
      not_finished = (m_threads.get(clnt->m_blockNo) == clnt);
      clnt->complete_poll();
    } while (not_finished);
  }
  return 0;
}

Uint32
TransporterFacade::open_clnt(trp_client * clnt, int blockNo)
{
  DBUG_ENTER("TransporterFacade::open");
  Guard g(m_open_close_mutex);
  if (DBG_POLL) ndbout_c("open(%p)", clnt);
  int r= m_threads.open(clnt);
  if (r < 0)
  {
    DBUG_RETURN(0);
  }

  if (unlikely(blockNo != -1))
  {
    // Using fixed block number, add fixed->dymamic mapping
    Uint32 fixed_index = blockNo - MIN_API_FIXED_BLOCK_NO;
    
    assert(blockNo >= MIN_API_FIXED_BLOCK_NO &&
           fixed_index <= NO_API_FIXED_BLOCKS);
    
    m_fixed2dynamic[fixed_index]= r;
  }

  if (theOwnId > 0)
  {
    r = numberToRef(r, theOwnId);
  }
  else
  {
    r = numberToRef(r, 0);
  }
  DBUG_RETURN(r);
}

TransporterFacade::~TransporterFacade()
{  
  DBUG_ENTER("TransporterFacade::~TransporterFacade");

  delete theClusterMgr;  
  NdbMutex_Lock(thePollMutex);
  delete theTransporterRegistry;
  NdbMutex_Unlock(thePollMutex);
  for (Uint32 i = 0; i < NDB_ARRAY_SIZE(m_send_buffers); i++)
  {
    NdbMutex_Deinit(&m_send_buffers[i].m_mutex);
  }
  NdbMutex_Destroy(thePollMutex);
  NdbMutex_Destroy(m_open_close_mutex);
  NdbMutex_Destroy(m_send_thread_mutex);
  NdbCondition_Destroy(m_send_thread_cond);
#ifdef API_TRACE
  signalLogger.setOutputStream(0);
#endif
  DBUG_VOID_RETURN;
}

void 
TransporterFacade::calculateSendLimit()
{
  Uint32 Ti;
  Uint32 TthreadCount = 0;
  
  Uint32 sz = m_threads.m_statusNext.size();
  for (Ti = 0; Ti < sz; Ti++) {
    if (m_threads.m_statusNext[Ti] == (ThreadData::ACTIVE)){
      TthreadCount++;
      m_threads.m_statusNext[Ti] = ThreadData::INACTIVE;
    }
  }
  currentSendLimit = TthreadCount;
  if (currentSendLimit == 0) {
    currentSendLimit = 1;
  }
  checkCounter = currentSendLimit << 2;
}


//-------------------------------------------------
// Force sending but still report the sending to the
// adaptive algorithm.
//-------------------------------------------------
void TransporterFacade::forceSend(Uint32 block_number) {
}

//-------------------------------------------------
// Improving API performance
//-------------------------------------------------
int
TransporterFacade::checkForceSend(Uint32 block_number) {  
  return 0;
}


/******************************************************************************
 * SEND SIGNAL METHODS
 *****************************************************************************/
int
TransporterFacade::sendSignal(trp_client* clnt,
                              const NdbApiSignal * aSignal, NodeId aNode)
{
  const Uint32* tDataPtr = aSignal->getConstDataPtrSend();
  Uint32 Tlen = aSignal->theLength;
  Uint32 TBno = aSignal->theReceiversBlockNumber;
#ifdef API_TRACE
  if(setSignalLog() && TRACE_GSN(aSignal->theVerId_signalNumber)){
    SignalHeader tmp = * aSignal;
    tmp.theSendersBlockRef = numberToRef(aSignal->theSendersBlockRef, theOwnId);
    LinearSectionPtr ptr[3];
    signalLogger.sendSignal(tmp,
                            1,
                            tDataPtr,
                            aNode, ptr, 0);
    signalLogger.flushSignalLog();
  }
#endif
  if ((Tlen != 0) && (Tlen <= 25) && (TBno != 0)) {
    SendStatus ss = theTransporterRegistry->prepareSend(clnt,
                                                        aSignal,
                                                        1, // JBB
                                                        tDataPtr,
                                                        aNode,
                                                        (LinearSectionPtr*)0);
    //if (ss != SEND_OK) ndbout << ss << endl;
    if (ss == SEND_OK)
    {
      assert(theClusterMgr->getNodeInfo(aNode).is_confirmed() ||
             aSignal->readSignalNumber() == GSN_API_REGREQ ||
             (aSignal->readSignalNumber() == GSN_CONNECT_REP &&
              aNode == ownId()) ||
             (aSignal->readSignalNumber() == GSN_CLOSE_COMREQ &&
              aNode == ownId()));
    }
    return (ss == SEND_OK ? 0 : -1);
  }
  else
  {
    ndbout << "ERR: SigLen = " << Tlen << " BlockRec = " << TBno;
    ndbout << " SignalNo = " << aSignal->theVerId_signalNumber << endl;
    assert(0);
  }//if
  return -1; // Node Dead
}

/**
 * FragmentedSectionIterator
 * -------------------------
 * This class acts as an adapter to a GenericSectionIterator
 * instance, providing a sub-range iterator interface.
 * It is used when long sections of a signal are fragmented
 * across multiple actual signals - the user-supplied
 * GenericSectionIterator is then adapted into a
 * GenericSectionIterator that only returns a subset of
 * the contained words for each signal fragment.
 */
class FragmentedSectionIterator: public GenericSectionIterator
{
private :
  GenericSectionIterator* realIterator; /* Real underlying iterator */
  Uint32 realIterWords;                 /* Total size of underlying */
  Uint32 realCurrPos;                   /* Current pos in underlying */
  Uint32 rangeStart;                    /* Sub range start in underlying */
  Uint32 rangeLen;                      /* Sub range len in underlying */
  Uint32 rangeRemain;                   /* Remaining words in underlying */
  const Uint32* lastReadPtr;            /* Ptr to last chunk obtained from
                                         * underlying */
  Uint32 lastReadPtrLen;                /* Remaining words in last chunk
                                         * obtained from underlying */
public:
  /* Constructor
   * The instance is constructed with the sub-range set to be the
   * full range of the underlying iterator
   */
  FragmentedSectionIterator(GenericSectionPtr ptr)
  {
    realIterator= ptr.sectionIter;
    realIterWords= ptr.sz;
    realCurrPos= 0;
    rangeStart= 0;
    rangeLen= rangeRemain= realIterWords;
    lastReadPtr= NULL;
    lastReadPtrLen= 0;
    moveToPos(0);

    assert(checkInvariants());
  }

private:
  /** 
   * checkInvariants
   * These class invariants must hold true at all stable states
   * of the iterator
   */
  bool checkInvariants()
  {
    assert( (realIterator != NULL) || (realIterWords == 0) );
    assert( realCurrPos <= realIterWords );
    assert( rangeStart <= realIterWords );
    assert( (rangeStart+rangeLen) <= realIterWords);
    assert( rangeRemain <= rangeLen );
    
    /* Can only have a null readptr if nothing is left */
    assert( (lastReadPtr != NULL) || (rangeRemain == 0));

    /* If we have a non-null readptr and some remaining 
     * words the readptr must have some words
     */
    assert( (lastReadPtr == NULL) || 
            ((rangeRemain == 0) || (lastReadPtrLen != 0)));
    return true;
  }

  /**
   * moveToPos
   * This method is used when the iterator is reset(), to move
   * to the start of the current sub-range.
   * If the iterator is already in-position then this is efficient
   * Otherwise, it has to reset() the underling iterator and
   * advance it until the start position is reached.
   */
  void moveToPos(Uint32 pos)
  {
    assert(pos <= realIterWords);

    if (pos < realCurrPos)
    {
      /* Need to reset, and advance from the start */
      realIterator->reset();
      realCurrPos= 0;
      lastReadPtr= NULL;
      lastReadPtrLen= 0;
    }

    if ((lastReadPtr == NULL) && 
        (realIterWords != 0) &&
        (pos != realIterWords))
      lastReadPtr= realIterator->getNextWords(lastReadPtrLen);
    
    if (pos == realCurrPos)
      return;

    /* Advance until we get a chunk which contains the pos */
    while (pos >= realCurrPos + lastReadPtrLen)
    {
      realCurrPos+= lastReadPtrLen;
      lastReadPtr= realIterator->getNextWords(lastReadPtrLen);
      assert(lastReadPtr != NULL);
    }

    const Uint32 chunkOffset= pos - realCurrPos;
    lastReadPtr+= chunkOffset;
    lastReadPtrLen-= chunkOffset;
    realCurrPos= pos;
  }

public:
  /**
   * setRange
   * Set the sub-range of the iterator.  Must be within the
   * bounds of the underlying iterator
   * After the range is set, the iterator is reset() to the
   * start of the supplied subrange
   */
  bool setRange(Uint32 start, Uint32 len)
  {
    assert(checkInvariants());
    if (start+len > realIterWords)
      return false;
    moveToPos(start);
    
    rangeStart= start;
    rangeLen= rangeRemain= len;

    assert(checkInvariants());
    return true;
  }

  /**
   * reset
   * (GenericSectionIterator)
   * Reset the iterator to the start of the current sub-range
   * Avoid calling as it could be expensive.
   */
  void reset()
  {
    /* Reset iterator to last specified range */
    assert(checkInvariants());
    moveToPos(rangeStart);
    rangeRemain= rangeLen;
    assert(checkInvariants());
  }

  /**
   * getNextWords
   * (GenericSectionIterator)
   * Get ptr and size of next contiguous words in subrange
   */
  const Uint32* getNextWords(Uint32& sz)
  {
    assert(checkInvariants());
    const Uint32* currPtr= NULL;

    if (rangeRemain)
    {
      assert(lastReadPtr != NULL);
      assert(lastReadPtrLen != 0);
      currPtr= lastReadPtr;
      
      sz= MIN(rangeRemain, lastReadPtrLen);
      
      if (sz == lastReadPtrLen)
        /* Will return everything in this chunk, move iterator to 
         * next
         */
        lastReadPtr= realIterator->getNextWords(lastReadPtrLen);
      else
      {
        /* Not returning all of this chunk, just advance within it */
        lastReadPtr+= sz;
        lastReadPtrLen-= sz;
      }
      realCurrPos+= sz;
      rangeRemain-= sz;
    }
    else
    {
      sz= 0;
    }
    
    assert(checkInvariants());
    return currPtr;
  }
};

/* Max fragmented signal chunk size (words) is max round number 
 * of NDB_SECTION_SEGMENT_SZ words with some slack left for 'main'
 * part of signal etc.
 */
#define CHUNK_SZ ((((MAX_SEND_MESSAGE_BYTESIZE >> 2) / NDB_SECTION_SEGMENT_SZ) - 2 ) \
                  * NDB_SECTION_SEGMENT_SZ)

/**
 * sendFragmentedSignal (GenericSectionPtr variant)
 * ------------------------------------------------
 * This method will send a signal with attached long sections.  If 
 * the signal is longer than CHUNK_SZ, the signal will be split into
 * multiple CHUNK_SZ fragments.
 * 
 * This is done by sending two or more long signals(fragments), with the
 * original GSN, but different signal data and with as much of the long 
 * sections as will fit in each.
 *
 * Non-final fragment signals contain a fraginfo value in the header
 * (1= first fragment, 2= intermediate fragment, 3= final fragment)
 * 
 * Fragment signals contain additional words in their signals :
 *   1..n words Mapping section numbers in fragment signal to original 
 *              signal section numbers
 *   1 word     Fragmented signal unique id.
 * 
 * Non final fragments (fraginfo=1/2) only have this data in them.  Final
 * fragments have this data in addition to the normal signal data.
 * 
 * Each fragment signal can transport one or more long sections, starting 
 * with section 0.  Sections are always split on NDB_SECTION_SEGMENT_SZ word
 * boundaries to simplify reassembly in the kernel.
 */
int
TransporterFacade::sendFragmentedSignal(trp_client* clnt,
                                        const NdbApiSignal* inputSignal,
                                        NodeId aNode,
                                        const GenericSectionPtr ptr[3],
                                        Uint32 secs)
{
  NdbApiSignal copySignal(* inputSignal);
  NdbApiSignal* aSignal = &copySignal;

  unsigned i;
  Uint32 totalSectionLength= 0;
  for (i= 0; i < secs; i++)
    totalSectionLength+= ptr[i].sz;
  
  /* If there's no need to fragment, send normally */
  if (totalSectionLength <= CHUNK_SZ)
    return sendSignal(clnt, aSignal, aNode, ptr, secs);
  
  // TODO : Consider tracing fragment signals?
#ifdef API_TRACE
  if(setSignalLog() && TRACE_GSN(aSignal->theVerId_signalNumber)){
    SignalHeader tmp = * aSignal;
    tmp.theSendersBlockRef = numberToRef(aSignal->theSendersBlockRef, theOwnId);
    signalLogger.sendSignal(tmp,
                            1,
                            aSignal->getConstDataPtrSend(),
                            aNode, ptr, 0);
    signalLogger.flushSignalLog();
    for (Uint32 i = 0; i<secs; i++)
      ptr[i].sectionIter->reset();
  }
#endif

  NdbApiSignal tmp_signal(*(SignalHeader*)aSignal);
  GenericSectionPtr tmp_ptr[3];
  GenericSectionPtr empty= {0, NULL};
  Uint32 unique_id= m_fragmented_signal_id++; // next unique id
  
  /* Init tmp_ptr array from ptr[] array, make sure we have
   * 0 length for missing sections
   */
  for (i= 0; i < 3; i++)
    tmp_ptr[i]= (i < secs)? ptr[i] : empty;

  /* Create our section iterator adapters */
  FragmentedSectionIterator sec0(tmp_ptr[0]);
  FragmentedSectionIterator sec1(tmp_ptr[1]);
  FragmentedSectionIterator sec2(tmp_ptr[2]);

  /* Replace caller's iterators with ours */
  tmp_ptr[0].sectionIter= &sec0;
  tmp_ptr[1].sectionIter= &sec1;
  tmp_ptr[2].sectionIter= &sec2;

  unsigned start_i= 0;
  unsigned this_chunk_sz= 0;
  unsigned fragment_info= 0;
  Uint32 *tmp_signal_data= tmp_signal.getDataPtrSend();
  for (i= 0; i < secs;) {
    unsigned remaining_sec_sz= tmp_ptr[i].sz;
    tmp_signal_data[i-start_i]= i;
    if (this_chunk_sz + remaining_sec_sz <= CHUNK_SZ)
    {
      /* This section fits whole, move onto next */
      this_chunk_sz+= remaining_sec_sz;
      i++;
      continue;
    }
    else
    {
      assert(this_chunk_sz <= CHUNK_SZ);
      /* This section doesn't fit, truncate it */
      unsigned send_sz= CHUNK_SZ - this_chunk_sz;
      if (i != start_i)
      {
        /* We ensure that the first piece of a new section which is
         * being truncated is a multiple of NDB_SECTION_SEGMENT_SZ
         * (to simplify reassembly).  Subsequent non-truncated pieces
         * will be CHUNK_SZ which is a multiple of NDB_SECTION_SEGMENT_SZ
         * The final piece does not need to be a multiple of
         * NDB_SECTION_SEGMENT_SZ
         * 
         * We round down the available send space to the nearest whole 
         * number of segments.
         * If there's not enough space for one segment, then we round up
         * to one segment.  This can make us send more than CHUNK_SZ, which
         * is ok as it's defined as less than the maximum message length.
         */
        send_sz = (send_sz / NDB_SECTION_SEGMENT_SZ) * 
          NDB_SECTION_SEGMENT_SZ;                        /* Round down */
        send_sz = MAX(send_sz, NDB_SECTION_SEGMENT_SZ);  /* At least one */
        send_sz = MIN(send_sz, remaining_sec_sz);        /* Only actual data */
        
        /* If we've squeezed the last bit of data in, jump out of 
         * here to send the last fragment.
         * Otherwise, send what we've collected so far.
         */
        if ((send_sz == remaining_sec_sz) &&      /* All sent */
            (i == secs - 1))                      /* No more sections */
        {
          this_chunk_sz+=  remaining_sec_sz;
          i++;
          continue;
        }
      }

      /* At this point, there must be data to send in a further signal */
      assert((send_sz < remaining_sec_sz) ||
             (i < secs - 1));

      /* Modify tmp generic section ptr to describe truncated
       * section
       */
      tmp_ptr[i].sz= send_sz;
      FragmentedSectionIterator* fragIter= 
        (FragmentedSectionIterator*) tmp_ptr[i].sectionIter;
      const Uint32 total_sec_sz= ptr[i].sz;
      const Uint32 start= (total_sec_sz - remaining_sec_sz);
      bool ok= fragIter->setRange(start, send_sz);
      assert(ok);
      if (!ok)
        return -1;
      
      if (fragment_info < 2) // 1 = first fragment signal
                             // 2 = middle fragments
	fragment_info++;

      // send tmp_signal
      tmp_signal_data[i-start_i+1]= unique_id;
      tmp_signal.setLength(i-start_i+2);
      tmp_signal.m_fragmentInfo= fragment_info;
      tmp_signal.m_noOfSections= i-start_i+1;
      // do prepare send
      {
	SendStatus ss = theTransporterRegistry->prepareSend
          (clnt,
           &tmp_signal,
	   1, /*JBB*/
	   tmp_signal_data,
	   aNode, 
	   &tmp_ptr[start_i]);
	assert(ss != SEND_MESSAGE_TOO_BIG);
	if (ss != SEND_OK) return -1;
        if (ss == SEND_OK)
        {
          assert(theClusterMgr->getNodeInfo(aNode).is_confirmed() ||
                 tmp_signal.readSignalNumber() == GSN_API_REGREQ);
        }
      }
      assert(remaining_sec_sz >= send_sz);
      Uint32 remaining= remaining_sec_sz - send_sz;
      tmp_ptr[i].sz= remaining;
      /* Set sub-range iterator to cover remaining words */
      ok= fragIter->setRange(start+send_sz, remaining);
      assert(ok);
      if (!ok)
        return -1;
      
      if (remaining == 0)
        /* This section's done, move onto the next */
	i++;
      
      // setup variables for next signal
      start_i= i;
      this_chunk_sz= 0;
    }
  }

  unsigned a_sz= aSignal->getLength();

  if (fragment_info > 0) {
    // update the original signal to include section info
    Uint32 *a_data= aSignal->getDataPtrSend();
    unsigned tmp_sz= i-start_i;
    memcpy(a_data+a_sz,
	   tmp_signal_data,
	   tmp_sz*sizeof(Uint32));
    a_data[a_sz+tmp_sz]= unique_id;
    aSignal->setLength(a_sz+tmp_sz+1);

    // send last fragment
    aSignal->m_fragmentInfo= 3; // 3 = last fragment
    aSignal->m_noOfSections= i-start_i;
  } else {
    aSignal->m_noOfSections= secs;
  }

  // send aSignal
  int ret;
  {
    SendStatus ss = theTransporterRegistry->prepareSend
      (clnt,
       aSignal,
       1/*JBB*/,
       aSignal->getConstDataPtrSend(),
       aNode,
       &tmp_ptr[start_i]);
    assert(ss != SEND_MESSAGE_TOO_BIG);
    if (ss == SEND_OK)
    {
      assert(theClusterMgr->getNodeInfo(aNode).is_confirmed() ||
             aSignal->readSignalNumber() == GSN_API_REGREQ);
    }
    ret = (ss == SEND_OK ? 0 : -1);
  }
  aSignal->m_noOfSections = 0;
  aSignal->m_fragmentInfo = 0;
  aSignal->setLength(a_sz);
  return ret;
}

int
TransporterFacade::sendFragmentedSignal(trp_client* clnt,
                                        const NdbApiSignal* aSignal,
                                        NodeId aNode,
                                        const LinearSectionPtr ptr[3],
                                        Uint32 secs)
{
  /* Use the GenericSection variant of sendFragmentedSignal */
  GenericSectionPtr tmpPtr[3];
  LinearSectionPtr linCopy[3];
  const LinearSectionPtr empty= {0, NULL};
  
  /* Make sure all of linCopy is initialised */
  for (Uint32 j=0; j<3; j++)
    linCopy[j]= (j < secs)? ptr[j] : empty;
  
  LinearSectionIterator zero (linCopy[0].p, linCopy[0].sz);
  LinearSectionIterator one  (linCopy[1].p, linCopy[1].sz);
  LinearSectionIterator two  (linCopy[2].p, linCopy[2].sz);

  /* Build GenericSectionPtr array using iterators */
  tmpPtr[0].sz= linCopy[0].sz;
  tmpPtr[0].sectionIter= &zero;
  tmpPtr[1].sz= linCopy[1].sz;
  tmpPtr[1].sectionIter= &one;
  tmpPtr[2].sz= linCopy[2].sz;
  tmpPtr[2].sectionIter= &two;

  return sendFragmentedSignal(clnt, aSignal, aNode, tmpPtr, secs);
}
  

int
TransporterFacade::sendSignal(trp_client* clnt,
                              const NdbApiSignal* aSignal, NodeId aNode,
                              const LinearSectionPtr ptr[3], Uint32 secs)
{
  Uint32 save = aSignal->m_noOfSections;
  const_cast<NdbApiSignal*>(aSignal)->m_noOfSections = secs;
#ifdef API_TRACE
  if(setSignalLog() && TRACE_GSN(aSignal->theVerId_signalNumber)){
    SignalHeader tmp = * aSignal;
    tmp.theSendersBlockRef = numberToRef(aSignal->theSendersBlockRef, theOwnId);
    signalLogger.sendSignal(tmp,
                            1,
                            aSignal->getConstDataPtrSend(),
                            aNode, ptr, secs);
    signalLogger.flushSignalLog();
  }
#endif
  SendStatus ss = theTransporterRegistry->prepareSend
    (clnt,
     aSignal,
     1, // JBB
     aSignal->getConstDataPtrSend(),
     aNode,
     ptr);
  assert(ss != SEND_MESSAGE_TOO_BIG);
  const_cast<NdbApiSignal*>(aSignal)->m_noOfSections = save;
  if (ss == SEND_OK)
  {
    assert(theClusterMgr->getNodeInfo(aNode).is_confirmed() ||
           aSignal->readSignalNumber() == GSN_API_REGREQ);
  }
  return (ss == SEND_OK ? 0 : -1);
}

int
TransporterFacade::sendSignal(trp_client* clnt,
                              const NdbApiSignal* aSignal, NodeId aNode,
                              const GenericSectionPtr ptr[3], Uint32 secs)
{
  Uint32 save = aSignal->m_noOfSections;
  const_cast<NdbApiSignal*>(aSignal)->m_noOfSections = secs;
#ifdef API_TRACE
  if(setSignalLog() && TRACE_GSN(aSignal->theVerId_signalNumber)){
    SignalHeader tmp = * aSignal;
    tmp.theSendersBlockRef = numberToRef(aSignal->theSendersBlockRef, theOwnId);
    signalLogger.sendSignal(tmp,
                            1,
                            aSignal->getConstDataPtrSend(),
                            aNode, ptr, secs);
    signalLogger.flushSignalLog();
    for (Uint32 i = 0; i<secs; i++)
      ptr[i].sectionIter->reset();
  }
#endif
  SendStatus ss = theTransporterRegistry->prepareSend
    (clnt,
     aSignal,
     1, // JBB
     aSignal->getConstDataPtrSend(),
     aNode,
     ptr);
  assert(ss != SEND_MESSAGE_TOO_BIG);
  const_cast<NdbApiSignal*>(aSignal)->m_noOfSections = save;
  if (ss == SEND_OK)
  {
    assert(theClusterMgr->getNodeInfo(aNode).is_confirmed() ||
           aSignal->readSignalNumber() == GSN_API_REGREQ);
  }
  return (ss == SEND_OK ? 0 : -1);
}

/******************************************************************************
 * CONNECTION METHODS  Etc
 ******************************************************************************/
void
TransporterFacade::doConnect(int aNodeId){
  theTransporterRegistry->setIOState(aNodeId, NoHalt);
  theTransporterRegistry->do_connect(aNodeId);
}

void
TransporterFacade::doDisconnect(int aNodeId)
{
  theTransporterRegistry->do_disconnect(aNodeId);
}

void
TransporterFacade::reportConnected(int aNodeId)
{
  theClusterMgr->reportConnected(aNodeId);
  return;
}

void
TransporterFacade::reportDisconnected(int aNodeId)
{
  theClusterMgr->reportDisconnected(aNodeId);
  return;
}

NodeId
TransporterFacade::ownId() const
{
  return theOwnId;
}

bool
TransporterFacade::isConnected(NodeId aNodeId){
  return theTransporterRegistry->is_connected(aNodeId);
}

NodeId
TransporterFacade::get_an_alive_node()
{
  DBUG_ENTER("TransporterFacade::get_an_alive_node");
  DBUG_PRINT("enter", ("theStartNodeId: %d", theStartNodeId));
#ifdef VM_TRACE
#ifdef NDB_USE_GET_ENV
  const char* p = NdbEnv_GetEnv("NDB_ALIVE_NODE_ID", (char*)0, 0);
  if (p != 0 && *p != 0)
    DBUG_RETURN(atoi(p));
#endif
#endif
  NodeId i;
  for (i = theStartNodeId; i < MAX_NDB_NODES; i++) {
    if (get_node_alive(i)){
      DBUG_PRINT("info", ("Node %d is alive", i));
      theStartNodeId = ((i + 1) % MAX_NDB_NODES);
      DBUG_RETURN(i);
    }
  }
  for (i = 1; i < theStartNodeId; i++) {
    if (get_node_alive(i)){
      DBUG_PRINT("info", ("Node %d is alive", i));
      theStartNodeId = ((i + 1) % MAX_NDB_NODES);
      DBUG_RETURN(i);
    }
  }
  DBUG_RETURN((NodeId)0);
}

TransporterFacade::ThreadData::ThreadData(Uint32 size){
  m_use_cnt = 0;
  m_firstFree = END_OF_LIST;
  expand(size);
}

void
TransporterFacade::ThreadData::expand(Uint32 size){
  trp_client * oe = 0;

  const Uint32 sz = m_statusNext.size();
  m_objectExecute.fill(sz + size, oe);
  for(Uint32 i = 0; i<size; i++){
    m_statusNext.push_back(sz + i + 1);
  }

  m_statusNext.back() = m_firstFree;
  m_firstFree = m_statusNext.size() - size;
}


int
TransporterFacade::ThreadData::open(trp_client * clnt)
{
  Uint32 nextFree = m_firstFree;

  if(m_statusNext.size() >= MAX_NO_THREADS && nextFree == END_OF_LIST){
    return -1;
  }

  if(nextFree == END_OF_LIST){
    expand(10);
    nextFree = m_firstFree;
  }

  m_use_cnt++;
  m_firstFree = m_statusNext[nextFree];

  m_statusNext[nextFree] = INACTIVE;
  m_objectExecute[nextFree] = clnt;

  return indexToNumber(nextFree);
}

int
TransporterFacade::ThreadData::close(int number){
  number= numberToIndex(number);
  assert(m_objectExecute[number] != 0);
  m_statusNext[number] = m_firstFree;
  assert(m_use_cnt);
  m_use_cnt--;
  m_firstFree = number;
  m_objectExecute[number] = 0;
  return 0;
}

Uint32
TransporterFacade::get_active_ndb_objects() const
{
  return m_threads.m_use_cnt;
}

void
TransporterFacade::start_poll(trp_client* clnt)
{
  assert(clnt->m_poll.m_locked == true);
  assert(clnt->m_poll.m_poll_owner == false);
  assert(clnt->m_poll.m_poll_queue == false);
  assert(clnt->m_poll.m_waiting == trp_client::PollQueue::PQ_IDLE);
}

bool
TransporterFacade::try_become_poll_owner(trp_client* clnt, Uint32 wait_time)
{
  lock_poll_mutex();
  if (m_poll_owner != NULL)
  {
    /*
      We didn't get hold of the poll "right". We will sleep on a
      conditional mutex until the thread owning the poll "right"
      will wake us up after all data is received. If no data arrives
      we will wake up eventually due to the timeout.
      After receiving all data we take the object out of the cond wait
      queue if it hasn't happened already. It is usually already out of the
      queue but at time-out it could be that the object is still there.
    */
    add_to_poll_queue(clnt);
    unlock_poll_mutex();
    dbg("cond_wait(%p)", clnt);
    NdbCondition_WaitTimeout(clnt->m_poll.m_condition,
                             clnt->m_mutex,
                             wait_time);

    switch(clnt->m_poll.m_waiting) {
    case trp_client::PollQueue::PQ_WOKEN:
      dbg("%p - PQ_WOKEN", clnt);
      // we have already been taken out of poll queue
      assert(clnt->m_poll.m_poll_queue == false);

      /**
       * clear m_poll_owner
       *   it can be that we were proposed as poll owner
       *   and later woken by another thread that became poll owner
       */
      clnt->m_poll.m_poll_owner = false;
      clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_IDLE;
      return false;
    case trp_client::PollQueue::PQ_IDLE:
      dbg("%p - PQ_IDLE", clnt);
      assert(false); // should not happen!!
      // ...treat as timeout...fall-through
    case trp_client::PollQueue::PQ_WAITING:
      dbg("%p - PQ_WAITING", clnt);
      break;
    }

    lock_poll_mutex();
    if (clnt->m_poll.m_poll_owner == false)
    {
      /**
       * We got timeout...hopefully rare...
       */
      assert(clnt->m_poll.m_poll_queue == true);
      remove_from_poll_queue(clnt);
      unlock_poll_mutex();
      clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_IDLE;
      dbg("%p - PQ_WAITING poll_owner == false => return", clnt);
      return false;
    }
    else if (m_poll_owner != 0)
    {
      /**
       * We were proposed as new poll owner...but someone else beat us too it
       *   break out...and retry the whole thing...
       */
      clnt->m_poll.m_poll_owner = false;
      assert(clnt->m_poll.m_poll_queue == false);
      unlock_poll_mutex();
      clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_IDLE;
      dbg("%p - PQ_WAITING m_poll_owner != 0 => return", clnt);
      return false;
    }

    /**
     * We were proposed as new poll owner, and was first to wakeup
     */
    dbg("%p - PQ_WAITING => new poll_owner", clnt);
  }
  m_poll_owner = clnt;
  unlock_poll_mutex();
  return true;
}

void
TransporterFacade::finish_poll(trp_client* clnt,
                               Uint32 cnt,
                               Uint32& cnt_woken,
                               trp_client** arr)
{
#ifndef NDEBUG
  {
    Uint32 lock_cnt = clnt->m_poll.m_locked_cnt;
    assert(lock_cnt >= 1);
    assert(lock_cnt <= clnt->m_poll.m_lock_array_size);
    assert(clnt->m_poll.m_locked_clients[0] == clnt);
    // no duplicates
    if (DBG_POLL) printf("after external_poll: cnt: %u ", lock_cnt);
    for (Uint32 i = 0; i < lock_cnt; i++)
    {
      trp_client * tmp = clnt->m_poll.m_locked_clients[i];
      if (DBG_POLL) printf("%p(%u) ", tmp, tmp->m_poll.m_waiting);
      for (Uint32 j = i + 1; j < lock_cnt; j++)
      {
        assert(tmp != clnt->m_poll.m_locked_clients[j]);
      }
    }
    if (DBG_POLL) printf("\n");

    for (Uint32 i = 1; i < lock_cnt; i++)
    {
      trp_client * tmp = clnt->m_poll.m_locked_clients[i];
      if (tmp->m_poll.m_locked == true)
      {
        assert(tmp->m_poll.m_waiting != trp_client::PollQueue::PQ_IDLE);
      }
      else
      {
        assert(tmp->m_poll.m_poll_owner == false);
        assert(tmp->m_poll.m_poll_queue == false);
        assert(tmp->m_poll.m_waiting == trp_client::PollQueue::PQ_IDLE);
      }
    }
  }
#endif

  /**
   * we're finished polling
   */
  clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_IDLE;

  /**
   * count woken clients
   *   and put them to the left in array
   */
  for (Uint32 i = 0; i < cnt; i++)
  {
    trp_client * tmp = arr[i];
    if (tmp->m_poll.m_waiting == trp_client::PollQueue::PQ_WOKEN)
    {
      arr[i] = arr[cnt_woken];
      arr[cnt_woken] = tmp;
      cnt_woken++;
    }
  }

  if (DBG_POLL)
  {
    Uint32 lock_cnt = clnt->m_poll.m_locked_cnt;
    printf("after sort: cnt: %u ", lock_cnt);
    for (Uint32 i = 0; i < lock_cnt; i++)
    {
      trp_client * tmp = clnt->m_poll.m_locked_clients[i];
      printf("%p(%u) ", tmp, tmp->m_poll.m_waiting);
    }
    printf("\n");
  }
}

void
TransporterFacade::try_lock_last_client(trp_client* clnt,
                                        bool& new_owner_locked,
                                        trp_client** new_owner_ptr,
                                        Uint32 first_check)
{
  /**
   * take last client in poll queue and try lock it
   */
  bool already_locked = false;
  trp_client* new_owner = remove_last_from_poll_queue();
  *new_owner_ptr = new_owner;
  assert(new_owner != clnt);
  if (new_owner != 0)
  {
    dbg("0 new_owner: %p", new_owner);
    /**
     * Note: we can only try lock here, to prevent potential deadlock
     *   given that we acquire mutex in different order when starting to poll
     *   Only lock if not already locked (can happen when signals received
     *   and trp_client isn't ready).
     */
    already_locked = clnt->m_poll.check_if_locked(new_owner, first_check);
    if ((!already_locked) &&
        (NdbMutex_Trylock(new_owner->m_mutex) != 0))
    {
      /**
       * If we fail to try lock...we put him back into poll-queue
       */
      new_owner_locked = false;
      add_to_poll_queue(new_owner);
      dbg("try-lock failed %p", new_owner);
    }
  }

  /**
   * clear poll owner variable and unlock
   */
  m_poll_owner = 0;
  unlock_poll_mutex();

  if (new_owner && new_owner_locked)
  {
    /**
     * Propose a poll owner
     *   Wakeup a client, that will race to become poll-owner
     *   I.e we don't assign m_poll_owner but let the wakeing up thread
     *   do this itself, if it is first
     */
    dbg("wake new_owner(%p)", new_owner);
#ifndef NDEBUG
    for (Uint32 i = 0; i < first_check; i++)
    {
      assert(clnt->m_poll.m_locked_clients[i] != new_owner);
    }
#endif
    assert(new_owner->m_poll.m_waiting == trp_client::PollQueue::PQ_WAITING);
    new_owner->m_poll.m_poll_owner = true;
    NdbCondition_Signal(new_owner->m_poll.m_condition);
    if (!already_locked)
    {
      /* Don't release lock if already locked */
      NdbMutex_Unlock(new_owner->m_mutex);
    }
  }
}

void
TransporterFacade::do_poll(trp_client* clnt,
                           Uint32 wait_time,
                           bool is_poll_owner,
                           bool stay_poll_owner)
{
  dbg("do_poll(%p)", clnt);
  clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_WAITING;
  assert(clnt->m_poll.m_locked == true);
  assert(clnt->m_poll.m_poll_owner == false);
  assert(clnt->m_poll.m_poll_queue == false);
  if (!is_poll_owner)
  {
    if (!try_become_poll_owner(clnt, wait_time))
      return;
  }

  /**
   * We have the poll "right" and we poll until data is received. After
   * receiving data we will check if all data is received,
   * if not we poll again.
   */
  clnt->m_poll.m_poll_owner = true;
  clnt->m_poll.start_poll(clnt);
  dbg("%p->external_poll", clnt);
  external_poll(wait_time);

  Uint32 cnt_woken = 0;
  Uint32 cnt = clnt->m_poll.m_locked_cnt - 1; // skip self
  trp_client ** arr = clnt->m_poll.m_locked_clients + 1; // skip self
  clnt->m_poll.m_poll_owner = false;
  finish_poll(clnt, cnt, cnt_woken, arr);

  lock_poll_mutex();

  if ((cnt + 1) > m_num_active_clients)
  {
    m_num_active_clients = cnt + 1;
  }
  /**
   * now remove all woken from poll queue
   * note: poll mutex held
   */
  remove_from_poll_queue(arr, cnt_woken);

  bool new_owner_locked = true;
  trp_client * new_owner = NULL;
  if (stay_poll_owner)
  {
    unlock_poll_mutex();
  }
  else
  {
    try_lock_last_client(clnt,
                         new_owner_locked,
                         &new_owner,
                         cnt_woken + 1);
  }

  /**
   * Now wake all the woken clients
   */
  unlock_and_signal(arr, cnt_woken);

  /**
   * And unlock the rest that we delivered messages to
   */
  for (Uint32 i = cnt_woken; i < cnt; i++)
  {
    dbg("unlock (%p)", arr[i]);
    NdbMutex_Unlock(arr[i]->m_mutex);
  }

  if (stay_poll_owner)
  {
    clnt->m_poll.m_locked_cnt = 0;
    dbg("%p->do_poll return", clnt);
    return;
  }
  /**
   * If we failed to propose new poll owner above, then we retry it here
   */
  if (new_owner_locked == false)
  {
    dbg("new_owner_locked == %s", "false");
    trp_client * new_owner;
    while (true)
    {
      new_owner = 0;
      lock_poll_mutex();
      if (m_poll_owner != 0)
      {
        /**
         * new poll owner already appointed...no need to do anything
         */
        break;
      }

      new_owner = remove_last_from_poll_queue();
      if (new_owner == 0)
      {
        /**
         * poll queue empty...no need to do anything
         */
        break;
      }

      if (NdbMutex_Trylock(new_owner->m_mutex) == 0)
      {
        /**
         * We locked a client that we will propose as poll owner
         */
        break;
      }

      /**
       * Failed to lock new owner, put him back on queue, and retry
       */
      add_to_poll_queue(new_owner);
      unlock_poll_mutex();
    }

    unlock_poll_mutex();

    if (new_owner)
    {
      /**
       * Propose a poll owner
       */
      assert(new_owner->m_poll.m_waiting == trp_client::PollQueue::PQ_WAITING);
      new_owner->m_poll.m_poll_owner = true;
      NdbCondition_Signal(new_owner->m_poll.m_condition);
      NdbMutex_Unlock(new_owner->m_mutex);
    }
  }

  clnt->m_poll.m_locked_cnt = 0;
  dbg("%p->do_poll return", clnt);
}

void
TransporterFacade::wakeup(trp_client* clnt)
{
  switch(clnt->m_poll.m_waiting) {
  case trp_client::PollQueue::PQ_WAITING:
    dbg("TransporterFacade::wakeup(%p) PQ_WAITING => PQ_WOKEN", clnt);
    clnt->m_poll.m_waiting = trp_client::PollQueue::PQ_WOKEN;
    break;
  case trp_client::PollQueue::PQ_WOKEN:
    dbg("TransporterFacade::wakeup(%p) PQ_WOKEN", clnt);
    break;
  case trp_client::PollQueue::PQ_IDLE:
    dbg("TransporterFacade::wakeup(%p) PQ_IDLE", clnt);
    break;
  }
}

void
TransporterFacade::unlock_and_signal(trp_client * const * arr, Uint32 cnt)
{
  for (Uint32 i = 0; i < cnt; i++)
  {
    NdbCondition_Signal(arr[i]->m_poll.m_condition);
    NdbMutex_Unlock(arr[i]->m_mutex);
  }
}

void
TransporterFacade::complete_poll(trp_client* clnt)
{
  assert(clnt->m_poll.m_poll_owner == false);
  assert(clnt->m_poll.m_poll_queue == false);
  assert(clnt->m_poll.m_waiting == trp_client::PollQueue::PQ_IDLE);
  clnt->flush_send_buffers();
}

void
trp_client::PollQueue::start_poll(trp_client* self)
{
  assert(m_waiting == PQ_WAITING);
  assert(m_locked);
  assert(m_poll_owner);
  assert(m_locked_cnt == 0);
  assert(&self->m_poll == this);
  m_locked_cnt = 1;
  m_locked_clients[0] = self;
}

bool
trp_client::PollQueue::check_if_locked(const trp_client* clnt,
                                       const Uint32 start) const
{
  for (Uint32 i = start; i<m_locked_cnt; i++)
  {
    if (m_locked_clients[i] == clnt) // already locked
      return true;
  }
  return false;
}

void
trp_client::PollQueue::lock_client (trp_client* clnt)
{
  assert(m_locked_cnt <= m_lock_array_size);
  if (check_if_locked(clnt, (Uint32)0))
    return;

  dbg("lock_client(%p)", clnt);

  assert(m_locked_cnt < m_lock_array_size);
  m_locked_clients[m_locked_cnt++] = clnt;
  NdbMutex_Lock(clnt->m_mutex);
  return;
}

void
TransporterFacade::add_to_poll_queue(trp_client* clnt)
{
  assert(clnt != 0);
  assert(clnt->m_poll.m_prev == 0);
  assert(clnt->m_poll.m_next == 0);
  assert(clnt->m_poll.m_locked == true);
  assert(clnt->m_poll.m_poll_owner == false);
  assert(clnt->m_poll.m_poll_queue == false);

  clnt->m_poll.m_poll_queue = true;
  if (m_poll_queue_head == 0)
  {
    assert(m_poll_queue_tail == 0);
    m_poll_queue_head = clnt;
    m_poll_queue_tail = clnt;
  }
  else
  {
    assert(m_poll_queue_tail->m_poll.m_next == 0);
    m_poll_queue_tail->m_poll.m_next = clnt;
    clnt->m_poll.m_prev = m_poll_queue_tail;
    m_poll_queue_tail = clnt;
  }
}

void
TransporterFacade::remove_from_poll_queue(trp_client* const * arr, Uint32 cnt)
{
  for (Uint32 i = 0; i< cnt; i++)
  {
    if (arr[i]->m_poll.m_poll_queue)
    {
      remove_from_poll_queue(arr[i]);
    }
  }
}

void
TransporterFacade::remove_from_poll_queue(trp_client* clnt)
{
  assert(clnt != 0);
  assert(clnt->m_poll.m_locked == true);
  assert(clnt->m_poll.m_poll_owner == false);
  assert(clnt->m_poll.m_poll_queue == true);

  clnt->m_poll.m_poll_queue = false;
  if (clnt->m_poll.m_prev != 0)
  {
    clnt->m_poll.m_prev->m_poll.m_next = clnt->m_poll.m_next;
  }
  else
  {
    assert(m_poll_queue_head == clnt);
    m_poll_queue_head = clnt->m_poll.m_next;
  }

  if (clnt->m_poll.m_next != 0)
  {
    clnt->m_poll.m_next->m_poll.m_prev = clnt->m_poll.m_prev;
  }
  else
  {
    assert(m_poll_queue_tail == clnt);
    m_poll_queue_tail = clnt->m_poll.m_prev;
  }

  if (m_poll_queue_head == 0)
    assert(m_poll_queue_tail == 0);
  else if (m_poll_queue_tail == 0)
    assert(m_poll_queue_head == 0);

  clnt->m_poll.m_prev = 0;
  clnt->m_poll.m_next = 0;
}

trp_client*
TransporterFacade::remove_last_from_poll_queue()
{
  trp_client * clnt = m_poll_queue_tail;
  if (clnt == 0)
    return 0;

  remove_from_poll_queue(clnt);
  return clnt;
}

template class Vector<trp_client*>;

#include "SignalSender.hpp"

const Uint32*
SignalSectionIterator::getNextWords(Uint32& sz)
{
  if (likely(currentSignal != NULL))
  {
    NdbApiSignal* signal= currentSignal;
    currentSignal= currentSignal->next();
    sz= signal->getLength();
    return signal->getDataPtrSend();
  }
  sz= 0;
  return NULL;
}

void
TransporterFacade::flush_send_buffer(Uint32 node, const TFBuffer * sb)
{
  Guard g(&m_send_buffers[node].m_mutex);
  assert(node < NDB_ARRAY_SIZE(m_send_buffers));
  link_buffer(&m_send_buffers[node].m_buffer, sb);
}

void
TransporterFacade::flush_and_send_buffer(Uint32 node, const TFBuffer * sb)
{
  assert(node < NDB_ARRAY_SIZE(m_send_buffers));
  struct TFSendBuffer * b = m_send_buffers + node;
  bool wake = false;
  NdbMutex_Lock(&b->m_mutex);
  link_buffer(&b->m_buffer, sb);

  if (b->m_sending == true)
  {
    /**
     * Sender will check if here is data, and wake send-thread
     * if needed
     */
  }
  else
  {
    b->m_sending = true;

    /**
     * Copy all data from m_buffer to m_out_buffer
     */
    TFBuffer copy = b->m_buffer;
    memset(&b->m_buffer, 0, sizeof(b->m_buffer));
    NdbMutex_Unlock(&b->m_mutex);
    link_buffer(&b->m_out_buffer, &copy);
    theTransporterRegistry->performSend(node);
    NdbMutex_Lock(&b->m_mutex);
    b->m_sending = false;
    if (b->m_buffer.m_bytes_in_buffer > 0 ||
        b->m_out_buffer.m_bytes_in_buffer > 0)
    {
      wake = true;
    }
  }
  NdbMutex_Unlock(&b->m_mutex);

  if (wake)
  {
    wakeup_send_thread();
  }
}

Uint32
TransporterFacade::get_bytes_to_send_iovec(NodeId node, struct iovec *dst,
                                           Uint32 max)
{
  if (max == 0)
  {
    return 0;
  }

  Uint32 count = 0;
  TFBuffer *b = &m_send_buffers[node].m_out_buffer;
  TFBufferGuard g0(* b);
  TFPage *page = b->m_head;
  while (page != NULL && count < max)
  {
    dst[count].iov_base = page->m_data+page->m_start;
    dst[count].iov_len = page->m_bytes;
    assert(page->m_start + page->m_bytes <= page->max_data_bytes());
    page = page->m_next;
    count++;
  }

  return count;
}

Uint32
TransporterFacade::bytes_sent(NodeId node, Uint32 bytes)
{
  TFBuffer *b = &m_send_buffers[node].m_out_buffer;
  TFBufferGuard g0(* b);
  Uint32 used_bytes = b->m_bytes_in_buffer;

  if (bytes == 0)
  {
    return used_bytes;
  }

  assert(used_bytes >= bytes);
  used_bytes -= bytes;
  b->m_bytes_in_buffer = used_bytes;

  TFPage *page = b->m_head;
  TFPage *prev = 0;
  while (bytes && bytes >= page->m_bytes)
  {
    prev = page;
    bytes -= page->m_bytes;
    page = page->m_next;
  }

  if (used_bytes == 0)
  {
    m_send_buffer.release(b->m_head, b->m_tail);
    b->m_head = 0;
    b->m_tail = 0;
  }
  else
  {
    if (prev)
    {
      m_send_buffer.release(b->m_head, prev);
    }

    page->m_start += bytes;
    page->m_bytes -= bytes;
    assert(page->m_start + page->m_bytes <= page->max_data_bytes());
    b->m_head = page;
  }

  return used_bytes;
}

bool
TransporterFacade::has_data_to_send(NodeId node)
{
  /**
   * Not used...
   */
  abort();
  return false;
}

void
TransporterFacade::reset_send_buffer(NodeId node, bool should_be_empty)
{
  // Make sure that buffer is already empty if the "should_be_empty"
  // flag is set. This is done to quickly catch any stray signals
  // written to the send buffer while not being connected
  TFBuffer *b0 = &m_send_buffers[node].m_buffer;
  TFBuffer *b1 = &m_send_buffers[node].m_out_buffer;
  bool has_data_to_send =
    (b0->m_head != NULL && b0->m_head->m_bytes) ||
    (b1->m_head != NULL && b1->m_head->m_bytes);

  if (should_be_empty && !has_data_to_send)
     return;
  assert(!should_be_empty);

  {
    TFBuffer *b = &m_send_buffers[node].m_buffer;
    if (b->m_head != 0)
    {
      m_send_buffer.release(b->m_head, b->m_tail);
    }
    memset(b, 0, sizeof(* b));
  }

  {
    TFBuffer *b = &m_send_buffers[node].m_out_buffer;
    if (b->m_head != 0)
    {
      m_send_buffer.release(b->m_head, b->m_tail);
    }
    memset(b, 0, sizeof(* b));
  }
}

#ifdef UNIT_TEST

// Unit test code starts
#include <random.h>

#define VERIFY(x) if ((x) == 0) { printf("VERIFY failed at Line %u : %s\n",__LINE__, #x);  return -1; }

/* Verify that word[n] == bias + n */
int
verifyIteratorContents(GenericSectionIterator& gsi, int dataWords, int bias)
{
  int pos= 0;

  while (pos < dataWords)
  {
    const Uint32* readPtr=NULL;
    Uint32 len= 0;

    readPtr= gsi.getNextWords(len);
    
    VERIFY(readPtr != NULL);
    VERIFY(len != 0);
    VERIFY(len <= (Uint32) (dataWords - pos));
    
    for (int j=0; j < (int) len; j++)
      VERIFY(readPtr[j] == (Uint32) (bias ++));

    pos += len;
  }

  return 0;
}

int
checkGenericSectionIterator(GenericSectionIterator& iter, int size, int bias)
{
  /* Verify contents */
  VERIFY(verifyIteratorContents(iter, size, bias) == 0);
  
  Uint32 sz;
  
  /* Check that iterator is empty now */
  VERIFY(iter.getNextWords(sz) == NULL);
  VERIFY(sz == 0);
    
  VERIFY(iter.getNextWords(sz) == NULL);
  VERIFY(sz == 0);
  
  iter.reset();
  
  /* Verify reset put us back to the start */
  VERIFY(verifyIteratorContents(iter, size, bias) == 0);
  
  /* Verify no more words available */
  VERIFY(iter.getNextWords(sz) == NULL);
  VERIFY(sz == 0);  
  
  return 0;
}

int
checkIterator(GenericSectionIterator& iter, int size, int bias)
{
  /* Test iterator itself, and then FragmentedSectionIterator
   * adaptation
   */
  VERIFY(checkGenericSectionIterator(iter, size, bias) == 0);
  
  /* Now we'll test the FragmentedSectionIterator on the iterator
   * we were passed
   */
  const int subranges= 20;
  
  iter.reset();
  GenericSectionPtr ptr;
  ptr.sz= size;
  ptr.sectionIter= &iter;
  FragmentedSectionIterator fsi(ptr);

  for (int s=0; s< subranges; s++)
  {
    Uint32 start= 0;
    Uint32 len= 0;
    if (size > 0)
    {
      start= (Uint32) myRandom48(size);
      if (0 != (size-start)) 
        len= (Uint32) myRandom48(size-start);
    }
    
    /*
      printf("Range (0-%u) = (%u + %u)\n",
              size, start, len);
    */
    fsi.setRange(start, len);
    VERIFY(checkGenericSectionIterator(fsi, len, bias + start) == 0);
  }
  
  return 0;
}



int
testLinearSectionIterator()
{
  /* Test Linear section iterator of various
   * lengths with section[n] == bias + n
   */
  const int totalSize= 200000;
  const int bias= 13;

  Uint32 data[totalSize];
  for (int i=0; i<totalSize; i++)
    data[i]= bias + i;

  for (int len= 0; len < 50000; len++)
  {
    LinearSectionIterator something(data, len);

    VERIFY(checkIterator(something, len, bias) == 0);
  }

  return 0;
}

NdbApiSignal*
createSignalChain(NdbApiSignal*& poolHead, int length, int bias)
{
  /* Create signal chain, with word[n] == bias+n */
  NdbApiSignal* chainHead= NULL;
  NdbApiSignal* chainTail= NULL;
  int pos= 0;
  int signals= 0;

  while (pos < length)
  {
    int offset= pos % NdbApiSignal::MaxSignalWords;
    
    if (offset == 0)
    {
      if (poolHead == NULL)
        return 0;

      NdbApiSignal* newSig= poolHead;
      poolHead= poolHead->next();
      signals++;

      newSig->next(NULL);

      if (chainHead == NULL)
      {
        chainHead= chainTail= newSig;
      }
      else
      {
        chainTail->next(newSig);
        chainTail= newSig;
      }
    }
    
    chainTail->getDataPtrSend()[offset]= (bias + pos);
    chainTail->setLength(offset + 1);
    pos ++;
  }

  return chainHead;
}
    
int
testSignalSectionIterator()
{
  /* Create a pool of signals, build
   * signal chains from it, test
   * the iterator against the signal chains
   */
  const int totalNumSignals= 1000;
  NdbApiSignal* poolHead= NULL;

  /* Allocate some signals */
  for (int i=0; i < totalNumSignals; i++)
  {
    NdbApiSignal* sig= new NdbApiSignal((BlockReference) 0);

    if (poolHead == NULL)
    {
      poolHead= sig;
      sig->next(NULL);
    }
    else
    {
      sig->next(poolHead);
      poolHead= sig;
    }
  }

  const int bias= 7;
  for (int dataWords= 1; 
       dataWords <= (int)(totalNumSignals * 
                          NdbApiSignal::MaxSignalWords); 
       dataWords ++)
  {
    NdbApiSignal* signalChain= NULL;
    
    VERIFY((signalChain= createSignalChain(poolHead, dataWords, bias)) != NULL );
    
    SignalSectionIterator ssi(signalChain);
    
    VERIFY(checkIterator(ssi, dataWords, bias) == 0);
    
    /* Now return the signals to the pool */
    while (signalChain != NULL)
    {
      NdbApiSignal* sig= signalChain;
      signalChain= signalChain->next();
      
      sig->next(poolHead);
      poolHead= sig;
    }
  }
  
  /* Free signals from pool */
  while (poolHead != NULL)
  {
    NdbApiSignal* sig= poolHead;
    poolHead= sig->next();
    delete(sig);
  }
  
  return 0;
}

int main(int arg, char** argv)
{
  /* Test Section Iterators
   * ----------------------
   * To run this code : 
   *   cd storage/ndb/src/ndbapi
   *   make testSectionIterators
   *   ./testSectionIterators
   *
   * Will print "OK" in success case
   */
  

  VERIFY(testLinearSectionIterator() == 0);
  VERIFY(testSignalSectionIterator() == 0);
  
  printf("OK\n");

  return 0;
}
#endif

void
TransporterFacade::set_auto_reconnect(int val)
{
  theClusterMgr->m_auto_reconnect = val;
}

int
TransporterFacade::get_auto_reconnect() const
{
  return theClusterMgr->m_auto_reconnect;
}

void
TransporterFacade::ext_set_max_api_reg_req_interval(Uint32 interval)
{
  theClusterMgr->set_max_api_reg_req_interval(interval);
}

void
TransporterFacade::ext_update_connections()
{
  theClusterMgr->lock();
  theTransporterRegistry->update_connections();
  theClusterMgr->flush_send_buffers();
  theClusterMgr->unlock();
}

struct in_addr
TransporterFacade::ext_get_connect_address(Uint32 nodeId)
{
  return theTransporterRegistry->get_connect_address(nodeId);
}

void
TransporterFacade::ext_forceHB()
{
  theClusterMgr->forceHB();
}

bool
TransporterFacade::ext_isConnected(NodeId aNodeId)
{
  bool val;
  theClusterMgr->lock();
  val = theClusterMgr->theNodes[aNodeId].is_connected();
  theClusterMgr->unlock();
  return val;
}

void
TransporterFacade::ext_doConnect(int aNodeId)
{
  theClusterMgr->lock();
  assert(theClusterMgr->theNodes[aNodeId].is_connected() == false);
  doConnect(aNodeId);
  theClusterMgr->unlock();
}

bool
TransporterFacade::setupWakeup()
{
  /* Ask TransporterRegistry to setup wakeup sockets */
  bool rc;
  lock_poll_mutex();
  {
    rc = theTransporterRegistry->setup_wakeup_socket();
  }
  unlock_poll_mutex();
  return rc;
}

bool
TransporterFacade::registerForWakeup(trp_client* _dozer)
{
  /* Called with Transporter lock */
  /* In future use a DLList for dozers.
   * Ideally with some way to wake one rather than all
   * For now, we just have one/TransporterFacade
   */
  if (dozer != NULL)
    return false;

  dozer = _dozer;
  return true;
}

bool
TransporterFacade::unregisterForWakeup(trp_client* _dozer)
{
  /* Called with Transporter lock */
  if (dozer != _dozer)
    return false;

  dozer = NULL;
  return true;
}

void
TransporterFacade::requestWakeup()
{
  /* Forward to TransporterRegistry
   * No need for locks, assuming only one client at a time will use
   */
  theTransporterRegistry->wakeup();
}


void
TransporterFacade::reportWakeup()
{
  /* Explicit wakeup callback
   * Called with Transporter Mutex held
   */
  /* Notify interested parties */
  if (dozer != NULL)
  {
    dozer->trp_wakeup();
  };
}
