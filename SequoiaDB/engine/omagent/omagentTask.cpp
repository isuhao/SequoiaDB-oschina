/*******************************************************************************


   Copyright (C) 2011-2014 SequoiaDB Ltd.

   This program is free software: you can redistribute it and/or modify
   it under the term of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warrenty of
   MARCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.

   Source File Name = omagentTask.cpp

   Dependencies: N/A

   Restrictions: N/A

   Change Activity:
   defect Date        Who Description
   ====== =========== === ==============================================
          08/06/2014  TZB Initial Draft

   Last Changed =

*******************************************************************************/

#include "ossTypes.h"
#include "omagentUtil.hpp"
#include "omagentTask.hpp"
#include "omagentJob.hpp"
#include "pmdDef.hpp"
#include "pmdEDU.hpp"
#include "omagentBackgroundCmd.hpp"
#include "omagentMgr.hpp"
#include <set>
#include <sstream>

namespace engine
{

   /*
      LOCAL DEFINE
   */

   #define OMA_TMP_COORD_NAME  "tmpCoord"
   
   #define OMA_WAIT_OMSVC_RES_TIMEOUT       ( 1 * OSS_ONE_SEC )
   #define OMA_WAIT_SUB_TASK_NOTIFY_TIMEOUT ( 3 * OSS_ONE_SEC )
   #define ADD_HOST_MAX_THREAD_NUM          10

   
   /*
      add host task
   */
   _omaAddHostTask::_omaAddHostTask( INT64 taskID )
   : _omaTask( taskID )
   {
      _taskType = OMA_TASK_ADD_HOST ;
      _taskName = OMA_TASK_NAME_ADD_HOST ;
      _eventID  = 0 ;
      _progress = 0 ;
      _errno    = SDB_OK ;
      ossMemset( _detail, 0, OMA_BUFF_SIZE + 1 ) ;
   }

   _omaAddHostTask::~_omaAddHostTask()
   {
   }

   INT32 _omaAddHostTask::init( const BSONObj &info, void *ptr )
   {
      INT32 rc = SDB_OK ;

      _addHostRawInfo = info.copy() ;
      
      PD_LOG ( PDDEBUG, "Add host passes argument: %s",
               _addHostRawInfo.toString( FALSE, TRUE ).c_str() ) ;

      rc = _initAddHostInfo( _addHostRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to get add host's info" ) ;
         goto error ;
      }
      _initAddHostResult() ;

      rc = initJsEnv() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to init environment for executing js script, "
                 "rc = %d", rc ) ;
      }

      done:
         return rc ;
      error:
         goto done ;
   }

   INT32 _omaAddHostTask::doit()
   {
      INT32 rc = SDB_OK ;

      setTaskStatus( OMA_TASK_STATUS_RUNNING ) ;

      rc = _checkHostInfo() ;
      if ( rc )
      {
         PD_LOG_MSG ( PDERROR, "Failed to check add host's info, "
                      "rc = %d", rc ) ;
         goto error ;
      }
      rc = _addHosts() ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to add hosts, rc = %d", rc ) ;
         goto error ;
      }
      
      rc = _waitAndUpdateProgress() ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to wait and update add host progress, "
                  "rc = %d", rc ) ;
         goto error ;
      }
      
   done:
      setTaskStatus( OMA_TASK_STATUS_FINISH ) ;
      
      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Failed to update add host progress"
                 "to omsvc, rc = %d", rc ) ;
      }
      sdbGetOMAgentMgr()->submitTaskInfo( _taskID ) ;
      
      PD_LOG( PDEVENT, "Omagent finish running add host task" ) ;
      
      return SDB_OK ;
   error:
      _setRetErr( rc ) ;
      goto done ;
   }

   AddHostInfo* _omaAddHostTask::getAddHostItem()
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      vector<AddHostInfo>::iterator it = _addHostInfo.begin() ;
      for( ; it != _addHostInfo.end(); it++ )
      {
         if ( FALSE == it->_flag )
         {
            it->_flag = TRUE ;
            return &(*it) ;
         }
      }
      return NULL ;
   }

   INT32 _omaAddHostTask::updateProgressToTask( INT32 serialNum,
                                                AddHostResultInfo &resultInfo )
   {
      INT32 rc            = SDB_OK ;
      INT32 totalNum      = 0 ;
      INT32 finishNum     = 0 ;
      
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
 
      map<INT32, AddHostResultInfo>::iterator it ;
      it = _addHostResult.find( serialNum ) ;
      if ( it != _addHostResult.end() )
      {
         PD_LOG( PDDEBUG, "No.%d add host sub task update progress to local "
                 "add host task. ip[%s], hostName[%s], status[%d], "
                 "statusDesc[%s], errno[%d], detail[%s], flow num[%d]",
                 serialNum, resultInfo._ip.c_str(),
                 resultInfo._hostName.c_str(),
                 resultInfo._status,
                 resultInfo._statusDesc.c_str(),
                 resultInfo._errno,
                 resultInfo._detail.c_str(),
                 resultInfo._flow.size() ) ;
         it->second = resultInfo ;
      }
      
      totalNum = _addHostResult.size() ;
      if ( 0 == totalNum )
      {
         rc = SDB_SYS ;
         PD_LOG_MSG( PDERROR, "Add host result is empty" ) ;
         goto error ;
      }
      it = _addHostResult.begin() ;
      for( ; it != _addHostResult.end(); it++ )
      {
         if ( OMA_TASK_STATUS_FINISH == it->second._status )
            finishNum++ ;
      }
      _progress = ( finishNum * 100 ) / totalNum ;

      _eventID++ ;
      _taskEvent.signal() ;

   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaAddHostTask::notifyUpdateProgress()
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      _eventID++ ;
      _taskEvent.signal() ;
   }

   void _omaAddHostTask::setErrInfo( INT32 errNum, const CHAR *pDetail )
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      if ( NULL == pDetail )
      {
         PD_LOG( PDWARNING, "Error detail is NULL" ) ;
         return ;
      }
      if ( ( SDB_OK == errNum) ||
           ( SDB_OK != _errno && '\0' != _detail[0] ) )
         return ;
      else
      {
         _errno = errNum ;
         ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
      }
   }

   INT32 _omaAddHostTask::_initAddHostInfo( BSONObj &info )
   {
      INT32 rc                   = SDB_OK ;
      const CHAR *pSdbUser       = NULL ;
      const CHAR *pSdbPasswd     = NULL ;
      const CHAR *pSdbUserGroup  = NULL ;
      const CHAR *pInstallPacket = NULL ;
      const CHAR *pStr           = NULL ;
      BSONObj hostInfoObj ;
      BSONElement ele ;

      ele = info.getField( OMA_FIELD_TASKID ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task id from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
       }
      _taskID = ele.numberLong() ;

      rc = omaGetObjElement( info, OMA_FIELD_INFO, hostInfoObj ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_INFO, rc ) ;
      rc = omaGetStringElement( hostInfoObj, OMA_FIELD_SDBUSER, &pSdbUser ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_SDBUSER, rc ) ;
      rc = omaGetStringElement( hostInfoObj, OMA_FIELD_SDBPASSWD,
                                &pSdbPasswd ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_SDBPASSWD, rc ) ;
      rc = omaGetStringElement( hostInfoObj, OMA_FIELD_SDBUSERGROUP,
                                &pSdbUserGroup ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_SDBUSERGROUP, rc ) ;
      rc = omaGetStringElement( hostInfoObj, OMA_FIELD_INSTALLPACKET,
                                &pInstallPacket ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_INSTALLPACKET, rc ) ;
      ele = hostInfoObj.getField( OMA_FIELD_HOSTINFO ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format add hosts"
                      "info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         INT32 serialNum = 0 ;
         while( itr.more() )
         {
            AddHostInfo hostInfo ;
            BSONObj item ;
            
            hostInfo._serialNum = serialNum++ ;
            hostInfo._flag      = FALSE ;
            hostInfo._taskID    = getTaskID() ;
            hostInfo._common._sdbUser = pSdbUser ;
            hostInfo._common._sdbPasswd = pSdbPasswd ;
            hostInfo._common._userGroup = pSdbUserGroup ;
            hostInfo._common._installPacket = pInstallPacket ;

            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            item = ele.embeddedObject() ;
            rc = omaGetStringElement( item, OMA_FIELD_IP, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_IP, rc ) ;
            hostInfo._item._ip = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_HOSTNAME, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_HOSTNAME, rc ) ;
            hostInfo._item._hostName = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_USER, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_USER, rc ) ;
            hostInfo._item._user = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_PASSWD, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_PASSWD, rc ) ;
            hostInfo._item._passwd = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_SSHPORT, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_SSHPORT, rc ) ;
            hostInfo._item._sshPort = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_AGENTSERVICE, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_AGENTSERVICE, rc ) ;
            hostInfo._item._agentService = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_INSTALLPATH, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_INSTALLPATH, rc ) ;
            hostInfo._item._installPath = pStr ;

            _addHostInfo.push_back( hostInfo ) ;
         }
      }
      
   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaAddHostTask::_initAddHostResult()
   {
      vector<AddHostInfo>::iterator itr = _addHostInfo.begin() ;

      for( ; itr != _addHostInfo.end(); itr++ )
      {
         AddHostResultInfo result ;
         result._ip         = itr->_item._ip ;
         result._hostName   = itr->_item._hostName ;
         result._status     = OMA_TASK_STATUS_INIT ;
         result._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_INIT ) ; ;
         result._errno      = SDB_OK ;
         result._detail     = "" ;
         
         _addHostResult.insert( std::pair< INT32, AddHostResultInfo >( 
            itr->_serialNum, result ) ) ;
      }
   }

   INT32 _omaAddHostTask::_checkHostInfo()
   {
      INT32 rc            = SDB_OK ;
      INT32 tmpRc         = SDB_OK ;
      INT32 errNum        = SDB_OK ;
      const CHAR *pDetail = NULL ;
      BSONObj retObj ;
      _omaCheckAddHostInfo runCmd ;

      rc = runCmd.init( _addHostRawInfo.objdata() ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to init to check add host's info,"
                  " rc = %d", rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to check add host's info" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to check add host's info,"
                  " rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "checking add host's info, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after checking "
                   "add host's info" ;
         goto error ;
      }
      if ( SDB_OK  != errNum )
      {
         rc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "checking add host's info, rc = %d", rc ) ;
            pDetail = "Failed to get errno detail from js after checking "
                      "add host's info" ;
            goto error ;
         }
         rc = errNum ;
         goto error ;
      }
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaAddHostTask::_addHosts()
   {
      INT32 rc        = SDB_OK ;
      INT32 threadNum = 0 ;
      INT32 hostNum   = _addHostInfo.size() ;
      
      if ( 0 == hostNum )
      {
         rc = SDB_INVALIDARG ;
         PD_LOG_MSG ( PDERROR, "No information for adding host" ) ;
         goto error ;
      }
      threadNum = hostNum < ADD_HOST_MAX_THREAD_NUM ? hostNum :
         ADD_HOST_MAX_THREAD_NUM ;
      for( INT32 i = 0; i < threadNum; i++ )
      { 
         ossScopedLock lock( &_taskLatch, EXCLUSIVE ) ;
         if ( TRUE == pmdGetThreadEDUCB()->isInterrupted() )
         {
            PD_LOG( PDEVENT, "Program has been interrupted, stop task[%s]",
                    _taskName.c_str() ) ;
            goto done ;
         }
         if ( OMA_TASK_STATUS_RUNNING == _taskStatus )
         {
            rc = startOmagentJob( OMA_TASK_ADD_HOST_SUB, _taskID,
                                  BSONObj(), (void *)this ) ;
            if ( rc )
            {
               PD_LOG_MSG ( PDERROR, "Failed to run add host sub task with the "
                            "type[%d], rc = %d", OMA_TASK_ADD_HOST_SUB, rc ) ;
               goto error ;
            }
         }
      }
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaAddHostTask::_waitAndUpdateProgress()
   {
      INT32 rc = SDB_OK ;
      BOOLEAN flag = FALSE ;
      UINT64 subTaskEventID = 0 ;
      _pmdEDUCB *cb = pmdGetThreadEDUCB () ;

      while ( !cb->isInterrupted() )
      {
         if ( SDB_OK != _taskEvent.wait ( OMA_WAIT_SUB_TASK_NOTIFY_TIMEOUT ) )
         {
            continue ;
         }
         else
         {
            while( TRUE )
            {
               _taskLatch.get() ;
               _taskEvent.reset() ;
               flag = ( subTaskEventID < _eventID ) ? TRUE : FALSE ;
               subTaskEventID = _eventID ;
               _taskLatch.release() ;
               if ( TRUE == flag )
               {
                  rc = _updateProgressToOM() ;
                  if ( SDB_APP_INTERRUPT == rc )
                  {
                     PD_LOG( PDERROR, "Failed to update add host progress"
                             " to omsvc, rc = %d", rc ) ;
                     goto error ;
                  }
                  else if ( SDB_OK != rc )
                  {
                     PD_LOG( PDERROR, "Failed to update add host progress"
                             " to omsvc, rc = %d", rc ) ;
                  }
               }
               else
               {
                  break ;
               }
            }
            if ( _isTaskFinish() )
            {
               PD_LOG( PDEVENT, "All the add host sub tasks had finished" ) ;
               goto done ;
            }
            
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when running add host task" ) ;
      rc = SDB_APP_INTERRUPT ;
    
   done:
      return rc ;
   error:
      goto done ; 
   }

   void _omaAddHostTask::_buildUpdateTaskObj( BSONObj &retObj )
   {
      BSONObjBuilder bob ;
      BSONArrayBuilder bab ;
      map<INT32, AddHostResultInfo>::iterator it = _addHostResult.begin() ;
      for ( ; it != _addHostResult.end(); it++ )
      {
         BSONObjBuilder builder ;
         BSONArrayBuilder arrBuilder ;
         BSONObj obj ;

         vector<string>::iterator itr = it->second._flow.begin() ;
         for ( ; itr != it->second._flow.end(); itr++ )
            arrBuilder.append( *itr ) ;
         
         builder.append( OMA_FIELD_IP, it->second._ip ) ;
         builder.append( OMA_FIELD_HOSTNAME, it->second._hostName ) ;
         builder.append( OMA_FIELD_STATUS, it->second._status ) ;
         builder.append( OMA_FIELD_STATUSDESC, it->second._statusDesc ) ;
         builder.append( OMA_FIELD_ERRNO, it->second._errno ) ;
         builder.append( OMA_FIELD_DETAIL, it->second._detail ) ;
         builder.append( OMA_FIELD_FLOW, arrBuilder.arr() ) ;
         obj = builder.obj() ;
         bab.append( obj ) ;
      }

      bob.appendNumber( OMA_FIELD_TASKID, _taskID ) ;
      if ( OMA_TASK_STATUS_FINISH == _taskStatus )
      {
         bob.appendNumber( OMA_FIELD_ERRNO, _errno ) ;
         bob.append( OMA_FIELD_DETAIL, _detail ) ;
      }
      else
      {
         bob.appendNumber( OMA_FIELD_ERRNO, SDB_OK ) ;
         bob.append( OMA_FIELD_DETAIL, "" ) ;
      }
      bob.appendNumber( OMA_FIELD_STATUS, _taskStatus ) ;
      bob.append( OMA_FIELD_STATUSDESC, getTaskStatusDesc( _taskStatus ) ) ;
      bob.appendNumber( OMA_FIELD_PROGRESS, _progress ) ;
      bob.appendArray( OMA_FIELD_RESULTINFO, bab.arr() ) ;

      retObj = bob.obj() ;
   }

   INT32 _omaAddHostTask::_updateProgressToOM()
   {
      INT32 rc            = SDB_OK ;
      INT32 retRc         = SDB_OK ;
      UINT64 reqID        = 0 ;
      omAgentMgr *pOmaMgr = sdbGetOMAgentMgr() ;
      _pmdEDUCB *cb       = pmdGetThreadEDUCB() ;
      ossAutoEvent updateEvent ;
      BSONObj obj ;
      
      _buildUpdateTaskObj( obj ) ;

      reqID = pOmaMgr->getRequestID() ;
      pOmaMgr->registerTaskEvent( reqID, &updateEvent ) ;
      
      while( !cb->isInterrupted() )
      {
         pOmaMgr->sendUpdateTaskReq( reqID, &obj ) ;
         while ( !cb->isInterrupted() )
         {
            if ( SDB_OK != updateEvent.wait( OMA_WAIT_OMSVC_RES_TIMEOUT, &retRc ) )
            {
               break ;
            }
            else
            {
               if ( SDB_OM_TASK_NOT_EXIST == retRc )
               {
                  PD_LOG( PDERROR, "Failed to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  rc = retRc ;
                  goto error ;
               }
               else if ( SDB_OK != retRc )
               {
                  PD_LOG( PDWARNING, "Retry to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  break ;
               }
               else
               {
                  PD_LOG( PDDEBUG, "Success to update task[%s]'s progress "
                          "with requestID[%lld]", _taskName.c_str(), reqID ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  goto done ;
               }
            }
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when update add host task "
              "progress to omsvc" ) ;
      rc = SDB_APP_INTERRUPT ;
      
   done:
      return rc ;
   error:
      goto done ;
   }

   BOOLEAN _omaAddHostTask::_isTaskFinish()
   {
      INT32 runNum    = 0 ;
      INT32 finishNum = 0 ;
      INT32 failNum   = 0 ;
      INT32 otherNum  = 0 ;
      BOOLEAN flag    = TRUE ;
      ossScopedLock lock( &_latch, EXCLUSIVE ) ;
      
      map< string, OMA_TASK_STATUS >::iterator it = _subTaskStatus.begin() ;
      for ( ; it != _subTaskStatus.end(); it++ )
      {
         switch ( it->second )
         {
         case OMA_TASK_STATUS_FINISH :
            finishNum++ ;
            break ;
         case OMA_TASK_STATUS_FAIL :            
            failNum++ ;
            break ;
         case OMA_TASK_STATUS_RUNNING :
            runNum++ ;
            flag = FALSE ;
            break ;
         default :
            otherNum++ ;
            flag = FALSE ;
            break ;
         }
      }
      PD_LOG( PDDEBUG, "In add host task, the amount of sub tasks is [%d]: "
              "[%d]running, [%d]finish, [%d]in the other status",
              _subTaskStatus.size(), runNum, finishNum, otherNum ) ;

      return flag ;
   }

   void _omaAddHostTask::_setRetErr( INT32 errNum )
   {
      const CHAR *pDetail = NULL ;

      if ( SDB_OK != _errno && '\0' != _detail[0] )
      {
         return ;
      }
      else
      {
         _errno = errNum ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL != pDetail && 0 != *pDetail )
         {
            ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
         }
         else
         {
            pDetail = getErrDesp( errNum ) ;
            if ( NULL != pDetail )
               ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
            else
               PD_LOG( PDERROR, "Failed to get error message" ) ;
         }
      }
   }

   /*
      remove host task
   */
   _omaRemoveHostTask::_omaRemoveHostTask( INT64 taskID )
   : _omaTask( taskID )
   {
      _taskType = OMA_TASK_REMOVE_HOST ;
      _taskName = OMA_TASK_NAME_REMOVE_HOST ;
      _progress = 0 ;
      _errno    = SDB_OK ;
      ossMemset( _detail, 0, OMA_BUFF_SIZE + 1 ) ;
   }

   _omaRemoveHostTask::~_omaRemoveHostTask()
   {
   }

   INT32 _omaRemoveHostTask::init( const BSONObj &info, void *ptr )
   {
      INT32 rc = SDB_OK ;

      _removeHostRawInfo = info.copy() ;
      
      PD_LOG ( PDDEBUG, "Remove host passes argument: %s",
               _removeHostRawInfo.toString( FALSE, TRUE ).c_str() ) ;

      rc = _initRemoveHostInfo( _removeHostRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to get remove host's info" ) ;
         goto error ;
      }
      _initRemoveHostResult() ;

      rc = initJsEnv() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to init environment for executing js script, "
                 "rc = %d", rc ) ;
      }

      done:
         return rc ;
      error:
         goto done ;
   }

   INT32 _omaRemoveHostTask::doit()
   {
      INT32 rc = SDB_OK ;

      setTaskStatus( OMA_TASK_STATUS_RUNNING ) ;

      rc = _removeHosts() ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to remove hosts, rc = %d", rc ) ;
         goto error ;
      }
      
   done:
      setTaskStatus( OMA_TASK_STATUS_FINISH ) ;
      
      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Failed to update remove host progress"
                 "to omsvc, rc = %d", rc ) ;
      }
      sdbGetOMAgentMgr()->submitTaskInfo( _taskID ) ;
      
      PD_LOG( PDEVENT, "Omagent finish running remove host task" ) ;
      
      return SDB_OK ;
   error:
      _setRetErr( rc ) ;
      goto done ;
   }

   INT32 _omaRemoveHostTask::updateProgressToTask( INT32 serialNum,
                                               RemoveHostResultInfo &resultInfo,
                                               BOOLEAN needToNotify )
   {
      INT32 rc            = SDB_OK ;
      INT32 totalNum      = 0 ;
      INT32 finishNum     = 0 ;
      
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
 
      map<INT32, RemoveHostResultInfo>::iterator it ;
      it = _removeHostResult.find( serialNum ) ;
      if ( it != _removeHostResult.end() )
      {
         PD_LOG( PDDEBUG, "Remove host update progress to local "
                 "task: ip[%s], hostName[%s], status[%d], "
                 "statusDesc[%s], errno[%d], detail[%s], flow num[%d]",
                 resultInfo._ip.c_str(),
                 resultInfo._hostName.c_str(),
                 resultInfo._status,
                 resultInfo._statusDesc.c_str(),
                 resultInfo._errno,
                 resultInfo._detail.c_str(),
                 resultInfo._flow.size() ) ;
         it->second = resultInfo ;
      }
      
      totalNum = _removeHostResult.size() ;
      if ( 0 == totalNum )
      {
         rc = SDB_SYS ;
         PD_LOG_MSG( PDERROR, "Remove host result is empty" ) ;
         goto error ;
      }
      it = _removeHostResult.begin() ;
      for( ; it != _removeHostResult.end(); it++ )
      {
         if ( OMA_TASK_STATUS_FINISH == it->second._status )
            finishNum++ ;
      }
      _progress = ( finishNum * 100 ) / totalNum ;

      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to update remove host progress"
                 "to omsvc, rc = %d", rc ) ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaRemoveHostTask::_initRemoveHostInfo( BSONObj &info )
   {
      INT32 rc                   = SDB_OK ;
      const CHAR *pStr           = NULL ;
      BSONObj hostInfoObj ;
      BSONElement ele ;

      ele = info.getField( OMA_FIELD_TASKID ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task id from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
       }
      _taskID = ele.numberLong() ;

      rc = omaGetObjElement( info, OMA_FIELD_INFO, hostInfoObj ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_INFO, rc ) ;
      ele = hostInfoObj.getField( OMA_FIELD_HOSTINFO ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format remove hosts"
                      "info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         INT32 serialNum = 0 ;
         while( itr.more() )
         {
            RemoveHostInfo hostInfo ;
            BSONObj item ;
            
            hostInfo._serialNum = serialNum++ ;
            hostInfo._taskID    = getTaskID() ;

            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            item = ele.embeddedObject() ;
            rc = omaGetStringElement( item, OMA_FIELD_IP, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_IP, rc ) ;
            hostInfo._item._ip = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_HOSTNAME, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_HOSTNAME, rc ) ;
            hostInfo._item._hostName = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_USER, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_USER, rc ) ;
            hostInfo._item._user = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_PASSWD, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_PASSWD, rc ) ;
            hostInfo._item._passwd = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_SSHPORT, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_SSHPORT, rc ) ;
            hostInfo._item._sshPort = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_CLUSTERNAME, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_CLUSTERNAME, rc ) ;
            hostInfo._item._clusterName = pStr ;
            rc = omaGetStringElement( item, OMA_FIELD_INSTALLPATH, &pStr ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d",
                      OMA_FIELD_INSTALLPATH, rc ) ;
            hostInfo._item._installPath = pStr ;

            _removeHostInfo.push_back( hostInfo ) ;
         }
      }
      
   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaRemoveHostTask::_initRemoveHostResult()
   {
      vector<RemoveHostInfo>::iterator itr = _removeHostInfo.begin() ;

      for( ; itr != _removeHostInfo.end(); itr++ )
      {
         RemoveHostResultInfo result ;
         result._ip         = itr->_item._ip ;
         result._hostName   = itr->_item._hostName ;
         result._status     = OMA_TASK_STATUS_INIT ;
         result._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_INIT ) ;
         result._errno      = SDB_OK ;
         result._detail     = "" ;
         
         _removeHostResult.insert( std::pair< INT32, RemoveHostResultInfo >( 
            itr->_serialNum, result ) ) ;
      }
   }

   INT32 _omaRemoveHostTask::_removeHosts()
   {
      INT32 rc      = SDB_OK ;
      INT32 tmpRc   = SDB_OK ;
      vector<RemoveHostInfo>::iterator it = _removeHostInfo.begin() ;
      
      for ( ; it != _removeHostInfo.end(); it++ )
      {
         RemoveHostResultInfo resultInfo = { "", "", OMA_TASK_STATUS_RUNNING,
                                             OMA_TASK_STATUS_DESC_RUNNING,
                                             SDB_OK, "" } ;
         CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
         const CHAR *pDetail          = NULL ;
         const CHAR *pIP              = NULL ;
         const CHAR *pHostName        = NULL ;
         INT32 errNum                 = 0 ;
         stringstream ss ;
         BSONObj retObj ;

         if ( TRUE == pmdGetThreadEDUCB()->isInterrupted() )
         {
            PD_LOG( PDEVENT, "Program has been interrupted, stop task[%s]",
                    _taskName.c_str() ) ;
            goto done ;
         }

         pIP                  = it->_item._ip.c_str() ;
         pHostName            = it->_item._hostName.c_str() ;
         resultInfo._ip       = pIP ;
         resultInfo._hostName = pHostName ;

         ossSnprintf( flow, OMA_BUFF_SIZE, "Removing host[%s]", pIP ) ;
         resultInfo._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( it->_serialNum, resultInfo, FALSE ) ;
         if ( tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update remove host[%s]'s progress, "
                    "rc = %d", pIP, tmpRc ) ;
         }

         _omaRemoveHost runCmd( *it ) ;
         rc = runCmd.init( NULL ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to init for removing "
                    "host[%s], rc = %d", pIP, rc ) ;
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init for removing host" ;
            goto build_error_result ;
         }
         rc = runCmd.doit( retObj ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to remove host[%s], rc = %d", pIP, rc ) ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( SDB_OK != tmpRc )
            {
               pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
               if ( NULL == pDetail || 0 == *pDetail )
                  pDetail = "Not exeute js file yet" ;
            }
            goto build_error_result ;
         }
         rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to get errno from js after"
                    " removing host[%s], rc = %d", pIP, rc ) ;
            ss << "Failed to get errno from js after"
                  " removing host[" << pIP << "]" ;
            pDetail = ss.str().c_str() ;
            goto build_error_result ;
         }
         if ( SDB_OK != errNum )
         {
            rc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( SDB_OK != tmpRc )
            {
               PD_LOG( PDERROR, "Failed to get error detail from js after "
                       "removing host[%s], rc = %d", pIP, tmpRc ) ;
               ss << "Failed to get error detail from js after"
                     " removing host[" << pIP << "]" ;
               pDetail = ss.str().c_str() ;
               goto build_error_result ;
            }
            rc = errNum ;
            goto build_error_result ;
         }
         else
         {
            ossSnprintf( flow, OMA_BUFF_SIZE, "Finish removing host[%s]", pIP ) ;
            PD_LOG ( PDEVENT, "Success to remove host[%s]", pIP ) ;
            resultInfo._status     = OMA_TASK_STATUS_FINISH ;
            resultInfo._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
            resultInfo._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( it->_serialNum, resultInfo, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update remove host[%s]'s progress, "
                       "rc = %d", pIP, tmpRc ) ;
            }
         }
         continue ; // if we success, nerver go to "build_error_result"
         
      build_error_result:
         ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to remove host[%s]", pIP ) ;
         resultInfo._status     = OMA_TASK_STATUS_FINISH ;
         resultInfo._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
         resultInfo._errno      = rc ;
         resultInfo._detail     = pDetail ;
         resultInfo._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( it->_serialNum, resultInfo, FALSE ) ;
         if ( tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update remove host[%s]'s progress, "
                    "rc = %d", pIP, tmpRc ) ;
         }
         continue ;
         
      }

   done:
      return SDB_OK ;
   }

   void _omaRemoveHostTask::_buildUpdateTaskObj( BSONObj &retObj )
   {
      BSONObjBuilder bob ;
      BSONArrayBuilder bab ;
      map<INT32, RemoveHostResultInfo>::iterator it = _removeHostResult.begin() ;
      for ( ; it != _removeHostResult.end(); it++ )
      {
         BSONObjBuilder builder ;
         BSONArrayBuilder arrBuilder ;
         BSONObj obj ;

         vector<string>::iterator itr = it->second._flow.begin() ;
         for ( ; itr != it->second._flow.end(); itr++ )
            arrBuilder.append( *itr ) ;
         
         builder.append( OMA_FIELD_IP, it->second._ip ) ;
         builder.append( OMA_FIELD_HOSTNAME, it->second._hostName ) ;
         builder.append( OMA_FIELD_STATUS, it->second._status ) ;
         builder.append( OMA_FIELD_STATUSDESC, it->second._statusDesc ) ;
         builder.append( OMA_FIELD_ERRNO, it->second._errno ) ;
         builder.append( OMA_FIELD_DETAIL, it->second._detail ) ;
         builder.append( OMA_FIELD_FLOW, arrBuilder.arr() ) ;
         obj = builder.obj() ;
         bab.append( obj ) ;
      }

      bob.appendNumber( OMA_FIELD_TASKID, _taskID ) ;
      if ( OMA_TASK_STATUS_FINISH == _taskStatus )
      {
         bob.appendNumber( OMA_FIELD_ERRNO, _errno ) ;
         bob.append( OMA_FIELD_DETAIL, _detail ) ;
      }
      else
      {
         bob.appendNumber( OMA_FIELD_ERRNO, SDB_OK ) ;
         bob.append( OMA_FIELD_DETAIL, "" ) ;
      }
      bob.appendNumber( OMA_FIELD_STATUS, _taskStatus ) ;
      bob.append( OMA_FIELD_STATUSDESC, getTaskStatusDesc( _taskStatus ) ) ;
      bob.appendNumber( OMA_FIELD_PROGRESS, _progress ) ;
      bob.appendArray( OMA_FIELD_RESULTINFO, bab.arr() ) ;

      retObj = bob.obj() ;
   }
   
   INT32 _omaRemoveHostTask::_updateProgressToOM()
   {
      INT32 rc            = SDB_OK ;
      INT32 retRc         = SDB_OK ;
      UINT64 reqID        = 0 ;
      omAgentMgr *pOmaMgr = sdbGetOMAgentMgr() ;
      _pmdEDUCB *cb       = pmdGetThreadEDUCB() ;
      ossAutoEvent updateEvent ;
      BSONObj obj ;
      
      _buildUpdateTaskObj( obj ) ;

      reqID = pOmaMgr->getRequestID() ;
      pOmaMgr->registerTaskEvent( reqID, &updateEvent ) ;
      
      while( !cb->isInterrupted() )
      {
         pOmaMgr->sendUpdateTaskReq( reqID, &obj ) ;
         while ( !cb->isInterrupted() )
         {
            if ( SDB_OK != updateEvent.wait( OMA_WAIT_OMSVC_RES_TIMEOUT, &retRc ) )
            {
               break ;
            }
            else
            {
               if ( SDB_OM_TASK_NOT_EXIST == retRc )
               {
                  PD_LOG( PDERROR, "Failed to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  rc = retRc ;
                  goto error ;
               }
               else if ( SDB_OK != retRc )
               {
                  PD_LOG( PDWARNING, "Retry to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  break ;
               }
               else
               {
                  PD_LOG( PDDEBUG, "Success to update task[%s]'s progress "
                          "with requestID[%lld]", _taskName.c_str(), reqID ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  goto done ;
               }
            }
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when update task[%s]'s "
              "progress to omsvc", _taskName.c_str() ) ;
      rc = SDB_APP_INTERRUPT ;
      
   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaRemoveHostTask::_setRetErr( INT32 errNum )
   {
      const CHAR *pDetail = NULL ;

      if ( SDB_OK != _errno && '\0' != _detail[0] )
      {
         return ;
      }
      else
      {
         _errno = errNum ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL != pDetail && 0 != *pDetail )
         {
            ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
         }
         else
         {
            pDetail = getErrDesp( errNum ) ;
            if ( NULL != pDetail )
               ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
            else
               PD_LOG( PDERROR, "Failed to get error message" ) ;
         }
      }
   }


   /*
      install db business task
   */
   _omaInstDBBusTask::_omaInstDBBusTask( INT64 taskID )
   : _omaTask( taskID )
   {
      _taskType      = OMA_TASK_INSTALL_DB ;
      _taskName      = OMA_TASK_NAME_INSTALL_DB_BUSINESS ;
      _isStandalone  = FALSE ;
      _nodeSerialNum = 0 ;
      _isTaskFail    = FALSE ;
      _eventID       = 0 ;
      _progress      = 0 ;
      _errno         = SDB_OK ;
      ossMemset( _detail, 0, OMA_BUFF_SIZE + 1 ) ;
   }

   _omaInstDBBusTask::~_omaInstDBBusTask()
   {
   }

   INT32 _omaInstDBBusTask::init( const BSONObj &info, void *ptr )
   {
      INT32 rc = SDB_OK ;

      _instDBBusRawInfo = info.copy() ;
      PD_LOG ( PDDEBUG, "Install db business passes argument: %s",
               _instDBBusRawInfo.toString( FALSE, TRUE ).c_str() ) ;

      rc = _initInstInfo( _instDBBusRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to get install db business info "
                 "rc = %d", rc ) ;
         goto error ;
      }
      rc = _initResultOrder( _instDBBusRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init result order rc = %d", rc ) ;
         goto error ;
      }

      rc = initJsEnv() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to init environment for executing js script, "
                 "rc = %d", rc ) ;
      }

      done:
         return rc ;
      error:
         goto done ;
   }

   INT32 _omaInstDBBusTask::doit()
   {
      INT32 rc                = SDB_OK ;
      BOOLEAN isTmpCoordOK    = FALSE ;
      BOOLEAN hasRollbackFail = FALSE ;

      if ( OMA_TASK_STATUS_ROLLBACK != _taskStatus )
      {
         setTaskStatus( OMA_TASK_STATUS_RUNNING ) ;
      }

      if ( _isStandalone )
      {
         rc = _installStandalone() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to install standalone, rc = %d", rc ) ;
            goto error ;
         }
      }
      else // in case of cluster
      {
         rc = _installTmpCoord() ;
         if ( SDB_OK == rc )
         {
            isTmpCoordOK = TRUE ;
            if ( OMA_TASK_STATUS_ROLLBACK == _taskStatus )
            {
               updateProgressToTask( SDB_OK, "", ROLE_DATA, OMA_TASK_STATUS_FINISH ) ;
               updateProgressToTask( SDB_OK, "", ROLE_COORD, OMA_TASK_STATUS_FINISH ) ;
               updateProgressToTask( SDB_OK, "", ROLE_CATA, OMA_TASK_STATUS_FINISH ) ;
               setErrInfo( SDB_OMA_TASK_FAIL, "Task Failed" ) ;
               goto done ;
            }
         }
         else
         {
            PD_LOG ( PDERROR, "Failed to install temporary coord, "
                     "rc = %d", rc ) ;
            goto error ;
         }
         rc = _installCatalog() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to create catalog, "
                    "rc = %d", rc ) ;
            goto error ;
         }
         rc = _installCoord() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to create coord, "
                    "rc = %d", rc ) ;
            goto error ;
         }
         rc = _installDataRG() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to create data groups, "
                    "rc = %d", rc ) ;
            goto error ;
         }

         rc = _waitAndUpdateProgress() ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to wait and update install db "
                     "business progress, rc = %d", rc ) ;
            goto error ;
         }
         if ( TRUE == _needToRollback() )
         {
            rc = SDB_OMA_TASK_FAIL ;
            PD_LOG ( PDEVENT, "Error happen, going to rollback" ) ;
            goto error ;
         }
      }
      
   done:
      setTaskStatus( OMA_TASK_STATUS_FINISH ) ;
      
      if ( FALSE == _isStandalone && FALSE == hasRollbackFail )
      {
         rc = _removeTmpCoord() ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "Failed to remove temporary coord, rc = %d", rc ) ;
         }
      }

      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Failed to update install db business progress "
                 "to omsvc, rc = %d", rc ) ;
      }
      
      sdbGetOMAgentMgr()->submitTaskInfo( _taskID ) ;
      
      PD_LOG( PDEVENT, "Omagent finish running install db business "
              "task[%lld]", _taskID ) ;
      return SDB_OK ;

   error:
      _setRetErr( rc );
      if ( TRUE == _isStandalone && SDBCM_NODE_EXISTED == rc )
         goto done ;
      if ( FALSE == _isStandalone && FALSE == isTmpCoordOK )
         goto done ;
      setTaskStatus( OMA_TASK_STATUS_ROLLBACK ) ;
      rc = _rollback() ;
      if ( rc )
      {
         hasRollbackFail = TRUE ;
         PD_LOG( PDERROR, "Failed to rollback install db business "
                 "task[%lld]", _taskID ) ;
      }
      goto done ;
      
   }

   void _omaInstDBBusTask::setIsTaskFail()
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      _isTaskFail = TRUE ;
   }

   BOOLEAN _omaInstDBBusTask::getIsTaskFail()
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      return _isTaskFail ;
   }

   string _omaInstDBBusTask::getTmpCoordSvcName()
   {
      return _tmpCoordSvcName ;
   }

   INT32 _omaInstDBBusTask::updateProgressToTask( INT32 serialNum,
                                                  InstDBResult &instResult,
                                                  BOOLEAN needToNotify )
   {
      INT32 rc            = SDB_OK ;
      vector<InstDBBusInfo>::iterator it ;
      map< string, vector<InstDBBusInfo> >::iterator it2 ;
      
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      PD_LOG( PDDEBUG, "Install db business update progress to local "
              "task: serialNum[%d], hostName[%s], svcName[%s], role[%s], "
              "groupName[%s], status[%d], statusDesc[%s], errno[%d], "
              "detail[%s], flow num[%d]",
              serialNum, instResult._hostName.c_str(),
              instResult._svcName.c_str(), instResult._role.c_str(),
              instResult._groupName.c_str(), instResult._status,
              instResult._statusDesc.c_str(), instResult._errno,
              instResult._detail.c_str(), instResult._flow.size() ) ;
 
      if ( TRUE == _isStandalone )
      {
         it = _standalone.begin() ;
         for ( ; it != _standalone.end(); it++ )
         {
            if ( serialNum == it->_nodeSerialNum )
            {
               it->_instResult = instResult ;
               break ;
            }
         }
      }
      else
      {
         if ( string(ROLE_DATA) == instResult._role )
         {
            it2 = _mapGroups.find( instResult._groupName ) ;
            if ( it2 != _mapGroups.end() )
            {
               it = it2->second.begin() ;
               for ( ; it != it2->second.end(); it++ )
               {
                  if ( serialNum == it->_nodeSerialNum )
                  {
                     it->_instResult = instResult ;
                     break ;
                  }
               }
            }
         }
         else if ( string(ROLE_COORD) == instResult._role )
         {
            it = _coord.begin() ;
            for ( ; it != _coord.end(); it++ )
            {
               if ( serialNum == it->_nodeSerialNum )
               {
                  it->_instResult = instResult ;
                  break ;
               }
            }
         }
         else if ( string(ROLE_CATA) == instResult._role )
         {
            it = _catalog.begin() ;
            for ( ; it != _catalog.end(); it++ )
            {
               if ( serialNum == it->_nodeSerialNum )
               {
                  it->_instResult = instResult ;
                  break ;
               }
            }
         }
         else
         {
            rc = SDB_SYS ;
            PD_LOG( PDWARNING, "Unknown role for updating progress when "
                    "installing node[%s:%s]",
                    instResult._hostName.c_str(),
                    instResult._svcName.c_str() ) ;
            goto error ;
         }
      }
      
      rc = _calculateProgress() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to calculate task's progress, "
                 "rc = %d", rc ) ;
      }

      if ( TRUE == needToNotify )
      {
         _eventID++ ;
         _taskEvent.signal() ;
      }
      else
      {
         rc = _updateProgressToOM() ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDWARNING, "Failed to update install db business progress"
                    "to omsvc, rc = %d", rc ) ;
         }
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::updateProgressToTask( INT32 errNum,
                                                  const CHAR *pDetail,
                                                  const CHAR *pRole,
                                                  OMA_TASK_STATUS status )
   {
      INT32 rc = SDB_OK ;
      string str ;
      string flow ;
      stringstream ss ;
      map< string, vector<InstDBBusInfo> >::iterator itr ;
      vector<InstDBBusInfo>::iterator it ;
#define BEGIN_ROLLBACK_GROUP  "Rollbacking "
#define FINISH_ROLLBACK_GROUP "Finish rollbacking "
#define FAIL_ROLLBACK_GROUP   "Failed to rollback "

      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      switch( status )
      {
      case OMA_TASK_STATUS_INIT:
      case OMA_TASK_STATUS_RUNNING:
         str = BEGIN_ROLLBACK_GROUP ;
         break ;
      case OMA_TASK_STATUS_FINISH:
         str = FINISH_ROLLBACK_GROUP ;
         break ;
      default:
         str = FAIL_ROLLBACK_GROUP ;
         status = OMA_TASK_STATUS_FINISH ;
         break ;
      }

      if ( 0 == ossStrncmp( pRole, ROLE_DATA, sizeof(ROLE_DATA) ) )
      {
         itr = _mapGroups.begin() ;
         for( ; itr != _mapGroups.end(); itr++ )
         {
            it = itr->second.begin() ;
            for ( ; it != itr->second.end(); it++ )
            {
               if ( SDB_OK == it->_instResult._errno )
               {
                  it->_instResult._errno = errNum ;
                  it->_instResult._detail = pDetail ;
               }
               it->_instResult._status = status ;
               it->_instResult._statusDesc = getTaskStatusDesc( status ) ;
               flow.clear() ;
               ss.str("") ;
               ss << str << "data node[" << it->_instInfo._hostName << ":" <<
                  it->_instInfo._svcName << "]" ;
               flow = ss.str() ;
               it->_instResult._flow.push_back( flow ) ;
            }
         }
      }
      else if ( 0 == ossStrncmp( pRole, ROLE_COORD, sizeof(ROLE_COORD) ) )
      {
         it = _coord.begin() ;
         for ( ; it != _coord.end(); it++ )
         {
            if ( SDB_OK == it->_instResult._errno )
            {
               it->_instResult._errno = errNum ;
               it->_instResult._detail = pDetail ;
            }
            it->_instResult._status = status ;
            it->_instResult._statusDesc = getTaskStatusDesc( status ) ;
            flow.clear() ;
            ss.str("") ;
            ss << str << "coord node[" << it->_instInfo._hostName << ":" <<
               it->_instInfo._svcName << "]" ;
            flow = ss.str() ;
            it->_instResult._flow.push_back( flow ) ;
         }
      }
      else if ( 0 == ossStrncmp( pRole, ROLE_CATA, sizeof(ROLE_CATA) ) )
      {
         it = _catalog.begin() ;
         for ( ; it != _catalog.end(); it++ )
         {
            if ( SDB_OK == it->_instResult._errno )
            {
               it->_instResult._errno = errNum ;
               it->_instResult._detail = pDetail ;
            }
            it->_instResult._status = status ;
            it->_instResult._statusDesc = getTaskStatusDesc( status ) ;
            flow.clear() ;
            ss.str("") ;
            ss << str << "catalog node[" << it->_instInfo._hostName << ":" <<
               it->_instInfo._svcName << "]" ;
            flow = ss.str() ;
            it->_instResult._flow.push_back( flow ) ;
         }
      }

      rc = _calculateProgress() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to calculate task's progress, "
                 "rc = %d", rc ) ;
      }
      
      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to update progress to omsvc, rc = %d", rc ) ;
      }
      
   return SDB_OK ;
   }

   void _omaInstDBBusTask::notifyUpdateProgress()
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      _eventID++ ;
      _taskEvent.signal() ;
   }

   void _omaInstDBBusTask::setErrInfo( INT32 errNum, const CHAR *pDetail )
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      if ( NULL == pDetail )
      {
         PD_LOG( PDWARNING, "Error detail is NULL" ) ;
         return ;
      }
      if ( ( SDB_OK == errNum ) ||
           ( SDB_OK != _errno && '\0' != _detail[0] ) )
         return ;
      else
      {
         _errno = errNum ;
         ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
      }
   }

   string _omaInstDBBusTask::getDataRGToInst()
   {
      string groupName ;
      map< string, vector<InstDBBusInfo> >::iterator it ;
      set<string>::iterator itr ;

      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      
      it = _mapGroups.begin() ;
      for ( ; it != _mapGroups.end(); it++ )
      {
         groupName = it->first ;
         itr = _existGroups.find( groupName ) ;
         if ( itr != _existGroups.end() )
         {
            groupName = "" ;
            continue ;
         }
         else
         {
            _existGroups.insert( groupName ) ;
            break ;
         }
      }
      
      return groupName ;
   }

   InstDBBusInfo* _omaInstDBBusTask::getDataNodeInfo( string &groupName )
   {
      InstDBBusInfo *pInstInfo = NULL ;
      map< string, vector<InstDBBusInfo> >::iterator it ;
      vector<InstDBBusInfo>::iterator itr ;

      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;
      
      it = _mapGroups.find( groupName ) ;
      if ( it != _mapGroups.end() )
      {
         itr = it->second.begin() ;
         for ( ; itr != it->second.end(); itr++ )
         {
            if ( OMA_TASK_STATUS_INIT == itr->_instResult._status )
            {
               itr->_instResult._status = OMA_TASK_STATUS_RUNNING ;
               itr->_instResult._statusDesc = getTaskStatusDesc( 
                                                  OMA_TASK_STATUS_RUNNING ) ;
               pInstInfo = &(*itr) ;
               break ;
            }
         }
      }
      
      return pInstInfo ;
   }

   INT32 _omaInstDBBusTask::_initInstInfo( BSONObj &info )
   {
      INT32 rc = SDB_OK ;
      BSONElement ele ;
      BSONObj hostInfoObj ;
      BSONObj filter ;
      BSONObj commonFileds ;
      BSONObjBuilder builder ;
      BSONObjBuilder builder2 ;
      BSONArrayBuilder bab ;
      string deplayMod ;
      const CHAR *pStr          = NULL ;
      const CHAR *pClusterName  = NULL ;
      const CHAR *pBusinessName = NULL ;
      

      ele = info.getField( OMA_FIELD_TASKID ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task id from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      _taskID = ele.numberLong() ;
      ele = info.getField( OMA_FIELD_STATUS ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task status from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      _taskStatus = (OMA_TASK_STATUS)ele.numberInt() ;

      rc = omaGetObjElement( info, OMA_FIELD_INFO, hostInfoObj ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_INFO, rc ) ;
      
      ele = hostInfoObj.getField( OMA_FIELD_DEPLOYMOD ) ;
      if ( String != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid content from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      deplayMod = ele.String() ;
      if ( deplayMod == string(DEPLAY_SA) )
      {
         _isStandalone = TRUE ;
      }
      else if ( deplayMod == string(DEPLAY_DB) )
      {
         _isStandalone = FALSE ;
      }
      else
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid deplay mode from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      
      builder.append( OMA_FIELD_USERTAG2, "" ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_CLUSTERNAME,
                                 &pClusterName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_CLUSTERNAME, rc ) ;
      builder.append( OMA_FIELD_CLUSTERNAME2, pClusterName ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_BUSINESSNAME,
                                 &pBusinessName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_BUSINESSNAME, rc ) ;
      builder.append( OMA_FIELD_BUSINESSNAME2, pBusinessName ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_SDBUSER, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_SDBUSER, rc ) ;
      builder.append( OMA_FIELD_SDBUSER, pStr ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_SDBPASSWD, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_SDBPASSWD, rc ) ;
      builder.append( OMA_FIELD_SDBPASSWD, pStr ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_SDBUSERGROUP, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_SDBUSERGROUP, rc ) ;
      builder.append( OMA_FIELD_SDBUSERGROUP, pStr ) ;
      commonFileds = builder.obj() ;

      builder2.append( OMA_FIELD_CLUSTERNAME2, pClusterName ) ;
      builder2.append( OMA_FIELD_BUSINESSNAME2, pBusinessName ) ;
      builder2.append( OMA_FIELD_USERTAG2, OMA_TMP_COORD_NAME ) ;
      builder2.appendArray( OMA_FIELD_CATAADDR, bab.arr() ) ;
      _tmpCoordCfgObj = builder2.obj() ;
      
      ele = hostInfoObj.getField ( OMA_FIELD_CONFIG ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format install "
                      "db business info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         while ( itr.more() )
         {
            InstDBBusInfo instDBBusInfo ;
            BSONObjBuilder bob ;
            BSONObj hostInfo ;
            BSONObj temp ;
            const CHAR *pRole = NULL ;
            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            temp = ele.embeddedObject() ;
            bob.appendElements( temp ) ;
            bob.appendElements( commonFileds ) ;
            hostInfo = bob.obj() ;
            rc = omaGetStringElement ( temp, OMA_OPTION_ROLE, &pRole ) ;
            if ( rc )
            {
               PD_LOG_MSG ( PDERROR, "Get field[%s] failed, rc = %d",
                            OMA_OPTION_ROLE, rc ) ;
               goto error ;
            }
            if ( 0 == ossStrncmp( pRole, ROLE_DATA,
                                  ossStrlen( ROLE_DATA ) ) )
            {
               string groupName = "" ;
               rc = omaGetStringElement( temp, OMA_FIELD_DATAGROUPNAME, &pStr ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Get field[%s] failed, rc: %d",
                         OMA_FIELD_DATAGROUPNAME, rc ) ;
               groupName = string( pStr ) ;
               rc = _initInstAndResultInfo( hostInfo, instDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init install db business info and result, "
                         "rc: %d", rc ) ;
               _mapGroups[groupName].push_back( instDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_COORD,
                                       ossStrlen( ROLE_COORD ) ) )
            {
               rc = _initInstAndResultInfo( hostInfo, instDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init install db business info and result, "
                         "rc: %d", rc ) ;
               _coord.push_back( instDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_CATA,
                                       ossStrlen( ROLE_CATA ) ) )
            {
               rc = _initInstAndResultInfo( hostInfo, instDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init install db business info and result, "
                         "rc: %d", rc ) ;
               _catalog.push_back( instDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_STANDALONE,
                                       ossStrlen( ROLE_STANDALONE ) ) )
            {
               rc = _initInstAndResultInfo( hostInfo, instDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init install db business info and result, "
                         "rc: %d", rc ) ;
               _standalone.push_back( instDBBusInfo ) ;
            }
            else
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG( PDERROR, "Unknown role for install db business" ) ;
               goto error ;
            }
         }
      }

   done:
      return rc ;
   error :
      goto done ;
   }

   INT32 _omaInstDBBusTask::_initInstAndResultInfo( BSONObj& hostInfo,
                                                    InstDBBusInfo &info )
   { 
      INT32 rc               = SDB_OK ; 
      const CHAR *pHostName  = NULL ;
      const CHAR *pSvcName   = NULL ;
      const CHAR *pGroupName = NULL ;
      const CHAR *pStr       = NULL ;
      BSONObj conf ;
      BSONObj pattern ;

      info._nodeSerialNum = _nodeSerialNum++ ;
      
      rc = omaGetStringElement( hostInfo, OMA_FIELD_HOSTNAME, &pHostName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_HOSTNAME, rc ) ;
      info._instInfo._hostName = pHostName ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_SVCNAME, &pSvcName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_OPTION_SVCNAME, rc ) ;
      info._instInfo._svcName = pSvcName ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_DBPATH, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_OPTION_DBPATH, rc ) ;
      info._instInfo._dbPath = pStr ;
      info._instInfo._confPath = "" ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_DATAGROUPNAME,
                                &pGroupName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_OPTION_DATAGROUPNAME, rc ) ;
      info._instInfo._dataGroupName = pGroupName ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_SDBUSER, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_SDBUSER, rc ) ;
      info._instInfo._sdbUser = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_SDBPASSWD, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_SDBPASSWD, rc ) ;
      info._instInfo._sdbPasswd = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_SDBUSERGROUP, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_SDBUSERGROUP, rc ) ;
      info._instInfo._sdbUserGroup = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_USER, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_USER, rc ) ;
      info._instInfo._user = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_PASSWD, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_PASSWD, rc ) ;
      info._instInfo._passwd = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_SSHPORT, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_SSHPORT, rc ) ;
      info._instInfo._sshPort = pStr ;
      pattern = BSON( OMA_FIELD_HOSTNAME       << 1 <<
                      OMA_OPTION_SVCNAME       << 1 <<
                      OMA_OPTION_DBPATH        << 1 <<
                      OMA_OPTION_DATAGROUPNAME << 1 <<
                      OMA_FIELD_SDBUSER        << 1 <<
                      OMA_FIELD_SDBPASSWD      << 1 << 
                      OMA_FIELD_SDBUSERGROUP   << 1 <<
                      OMA_FIELD_USER           << 1 <<
                      OMA_FIELD_PASSWD         << 1 <<
                      OMA_FIELD_SSHPORT        << 1 ) ;
      conf = hostInfo.filterFieldsUndotted( pattern, false ) ;
      info._instInfo._conf = conf.copy() ;

      rc = omaGetStringElement( hostInfo, OMA_OPTION_ROLE, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_OPTION_ROLE, rc ) ;
      info._instResult._errno      = SDB_OK ;
      info._instResult._detail     = "" ;
      info._instResult._hostName   = pHostName ;
      info._instResult._svcName    = pSvcName ;
      info._instResult._role       = pStr ;
      info._instResult._groupName  = pGroupName ;
      info._instResult._status     = OMA_TASK_STATUS_INIT ;
      info._instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_INIT ) ;

   done:
      return rc ;
   error:
      goto done ;

   }

   INT32 _omaInstDBBusTask::_initResultOrder( BSONObj &info )
   {
      INT32 rc = SDB_OK ;
      BSONElement ele ;
      const CHAR *pHostName = NULL ;
      const CHAR *pSvcName  = NULL ;

      ele = info.getField ( OMA_FIELD_RESULTINFO ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format install "
                      "db business info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         
         while ( itr.more() )
         {
            BSONObj resultInfo ;
            pair<string, string> p ;
            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            resultInfo = ele.embeddedObject() ;
            rc = omaGetStringElement( resultInfo, OMA_FIELD_HOSTNAME, &pHostName ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d", OMA_FIELD_HOSTNAME, rc ) ;
            rc = omaGetStringElement( resultInfo, OMA_FIELD_SVCNAME, &pSvcName ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d", OMA_FIELD_SVCNAME, rc ) ;
            p.first = pHostName ;
            p.second = pSvcName ;
            _resultOrder.push_back( p ) ;
         }
      }
      
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_saveTmpCoordInfo( BSONObj &info )
   {
      INT32 rc         = SDB_OK ;
      const CHAR *pStr = NULL ;
      rc = omaGetStringElement( info, OMA_FIELD_TMPCOORDSVCNAME, &pStr ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to get filed[%s], rc = %s",
                  OMA_FIELD_TMPCOORDSVCNAME, rc ) ;
         goto error ;
      }
      _tmpCoordSvcName = pStr ;
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_installTmpCoord()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      const CHAR *pDetail          = NULL ;
      BSONObj retObj ;
      _omaCreateTmpCoord runCmd( _taskID ) ;
      
      rc = runCmd.createTmpCoord( _tmpCoordCfgObj, retObj ) ;
      if ( rc )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Install temporary coord does not execute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after"
                 " installing temporay coord, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after installing"
                   " temporary coord" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after"
                    " installing temporay coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after"
                       " installing temporay coord" ;
            goto done ;
         }
         rc = errNum ;
         goto error ;
      }
      rc = _saveTmpCoordInfo( retObj ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to save installed temporary coord's "
                  "info, rc = %d", rc ) ;
         pDetail = "Failed to save installed temporary coord's info" ;
         goto error ;
      }
      
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaInstDBBusTask::_removeTmpCoord()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      const CHAR *pDetail          = NULL ;
      BSONObj retObj ;
      _omaRemoveTmpCoord runCmd( _taskID, _tmpCoordSvcName ) ;
      
      rc = runCmd.removeTmpCoord ( retObj ) ;
      if ( rc )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Remove temporary coord does not execute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "removing temporay coord, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after removing temporay coord" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         tmpRc = omaGetStringElement( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "removing temporay coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "removing temporay coord" ;
            goto error ;
         }
         rc = errNum ;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_installStandalone()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
      const CHAR *pDetail          = "" ;
      INT32 errNum                 = 0 ;
      vector<InstDBBusInfo>::iterator itr = _standalone.begin() ;

      for ( ; itr != _standalone.end(); itr++ )
      {
         BSONObj retObj ;
         InstDBResult instResult = itr->_instResult ;
         _omaInstallStandalone runCmd( _taskID, itr->_instInfo ) ;
         const CHAR *pHostName = itr->_instInfo._hostName.c_str() ;
         const CHAR *pSvcName  = itr->_instInfo._svcName.c_str() ;
         
         ossSnprintf( flow, OMA_BUFF_SIZE, "Installing standalone[%s:%s]",
                      pHostName, pSvcName ) ;
         instResult._status = OMA_TASK_STATUS_RUNNING ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_RUNNING ) ;
         instResult._flow.push_back( flow ) ;
         rc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to update progress before install "
                     "standalone, rc = %d", rc ) ;
            goto error ;
         }
         rc = runCmd.init( NULL ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to init to install standalone[%s:%s], "
                    "rc = %d", pHostName, pSvcName, rc ) ;
            
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init to install standalone" ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "standalone[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
            instResult._errno      = rc ;
            instResult._detail     = pDetail ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( SDB_OK != tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update install standalone[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }         
            goto error ;
         }
         rc = runCmd.doit( retObj ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to install standalone[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( tmpRc )
            {
               pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
               if ( NULL == pDetail || 0 == *pDetail )
                  pDetail = "Not exeute js file yet" ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "standalone[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
            instResult._errno      = rc ;
            instResult._detail     = pDetail ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update install standalone[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, rc ) ;
            }
            goto error ;
         }
         rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to get errno from js after "
                    "installing standalone[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            pDetail = "Failed to get errno from js after installing standalone" ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "standalone[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
            instResult._errno      = rc ;
            instResult._detail     = pDetail ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to uupdate install standalone[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }
            goto error ;
         }
         if ( SDB_OK != errNum )
         {
            rc = errNum ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( SDB_OK != tmpRc )
            {
               PD_LOG( PDERROR, "Failed to get error detail from js after "
                       "installing standalone[%s:%s], rc = %d",
                       pHostName, pSvcName, tmpRc ) ;
               pDetail = "Failed to get error detail from js after "
                         "installing standalone" ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "standalone[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
            instResult._errno      = errNum ;
            instResult._detail     = pDetail ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to uupdate install standalone[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }
            goto error ;
         }
         else
         {

            ossSnprintf( flow, OMA_BUFF_SIZE,
                         "Finish installing standalone[%s:%s]",
                         pHostName, pSvcName ) ;
            PD_LOG ( PDEVENT, "Success to install standalone[%s:%s]",
                     pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_FINISH ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update install standalone[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }
         }
      }
      
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaInstDBBusTask::_installCatalog()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
      const CHAR *pDetail          = "" ;
      INT32 errNum                 = 0 ;
      vector<InstDBBusInfo>::iterator itr = _catalog.begin() ;

      for ( ; itr != _catalog.end(); itr++ )
      {
         BSONObj retObj ;
         stringstream ss ;
         InstDBResult instResult = itr->_instResult ;
         _omaInstallCatalog runCmd( _taskID, _tmpCoordSvcName, itr->_instInfo ) ;
         const CHAR *pHostName = itr->_instInfo._hostName.c_str() ;
         const CHAR *pSvcName  = itr->_instInfo._svcName.c_str() ;

         ossSnprintf( flow, OMA_BUFF_SIZE, "Installing catalog[%s:%s]",
                      pHostName, pSvcName ) ;
         instResult._status = OMA_TASK_STATUS_RUNNING ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_RUNNING ) ;
         instResult._flow.push_back( flow ) ;
         rc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to update progress before install "
                     "catalog, rc = %d", rc ) ;
            goto error ;
         }
         rc = runCmd.init( NULL ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to init to install catalog[%s:%s], "
                    "rc = %d", pHostName, pSvcName, rc ) ;
            
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init to install catalog" ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "catalog[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         rc = runCmd.doit( retObj ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to install catalog[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( tmpRc )
            {
               pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
               if ( NULL == pDetail || 0 == *pDetail )
                  pDetail = "Not exeute js file yet" ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "catalog[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to get errno from js after "
                    "installing catalog[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            ss << "Failed to get errno from js after installing catalog[" <<
               pHostName << ":" << pSvcName << "]" ;
            pDetail = ss.str().c_str() ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "catalog[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         if ( SDB_OK != errNum )
         {
            rc = errNum ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( SDB_OK != tmpRc )
            {
               PD_LOG( PDERROR, "Failed to get error detail from js after "
                       "installing catalog[%s:%s], rc = %d",
                       pHostName, pSvcName, tmpRc ) ;
               ss << "Failed to get error detail from js after "
                     "installing catalog[" << pHostName << ":" << pSvcName << "]" ; 
               pDetail = ss.str().c_str() ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "catalog[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         else
         {

            ossSnprintf( flow, OMA_BUFF_SIZE,
                         "Finish installing catalog[%s:%s]",
                         pHostName, pSvcName ) ;
            PD_LOG ( PDEVENT, "Success to install catalog[%s:%s]",
                     pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_FINISH ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update install catalog[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }
         }
         continue ; // if we success, nerver go to "build_error_result"
      
      build_error_result:
         instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
         instResult._errno      = rc ;
         instResult._detail     = pDetail ;
         instResult._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update install catalog[%s:%s]'s "
                    "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
         }
         goto error ;
      }
      
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaInstDBBusTask::_installCoord()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
      const CHAR *pDetail          = "" ;
      INT32 errNum                 = 0 ;
      vector<InstDBBusInfo>::iterator itr = _coord.begin() ;

      for ( ; itr != _coord.end(); itr++ )
      {
         BSONObj retObj ;
         stringstream ss ;
         InstDBResult instResult = itr->_instResult ;
         _omaInstallCoord runCmd( _taskID, _tmpCoordSvcName, itr->_instInfo ) ;
         const CHAR *pHostName = itr->_instInfo._hostName.c_str() ;
         const CHAR *pSvcName  = itr->_instInfo._svcName.c_str() ;

         ossSnprintf( flow, OMA_BUFF_SIZE, "Installing coord[%s:%s]",
                      pHostName, pSvcName ) ;
         instResult._status = OMA_TASK_STATUS_RUNNING ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_RUNNING ) ;
         instResult._flow.push_back( flow ) ;
         rc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to update progress before install "
                     "coord, rc = %d", rc ) ;
            goto error ;
         }
         rc = runCmd.init( NULL ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to init to install coord[%s:%s], "
                    "rc = %d", pHostName, pSvcName, rc ) ;
            
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init to install coord" ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "coord[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         rc = runCmd.doit( retObj ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to install coord[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( tmpRc )
            {
               pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
               if ( NULL == pDetail || 0 == *pDetail )
                  pDetail = "Not exeute js file yet" ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "coord[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to get errno from js after "
                    "installing coord[%s:%s], rc = %d",
                    pHostName, pSvcName, rc ) ;
            ss << "Failed to get errno from js after installing coord[" <<
               pHostName << ":" << pSvcName << "]" ;
            pDetail = ss.str().c_str() ;
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "coord[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         if ( SDB_OK != errNum )
         {
            rc = errNum ;
            tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
            if ( SDB_OK != tmpRc )
            {
               PD_LOG( PDERROR, "Failed to get error detail from js after "
                       "installing coord[%s:%s], rc = %d",
                       pHostName, pSvcName, tmpRc ) ;
               ss << "Failed to get error detail from js after "
                     "installing coord[" << pHostName << ":" << pSvcName << "]" ;
               pDetail = ss.str().c_str() ;
            }
            ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to install "
                         "coord[%s:%s], going to rollback",
                         pHostName, pSvcName ) ;
            goto build_error_result ;
         }
         else
         {

            ossSnprintf( flow, OMA_BUFF_SIZE,
                         "Finish installing coord[%s:%s]",
                         pHostName, pSvcName ) ;
            PD_LOG ( PDEVENT, "Success to install coord[%s:%s]",
                     pHostName, pSvcName ) ;
            instResult._status     = OMA_TASK_STATUS_FINISH ;
            instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
            instResult._flow.push_back( flow ) ;
            tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
            if ( tmpRc )
            {
               PD_LOG( PDWARNING, "Failed to update install coord[%s:%s]'s "
                       "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
            }
         }
         continue ; // if we success, nerver go to "build_error_result"
      
      build_error_result:
         instResult._status     = OMA_TASK_STATUS_ROLLBACK ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_ROLLBACK ) ;
         instResult._errno      = rc ;
         instResult._detail     = pDetail ;
         instResult._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( itr->_nodeSerialNum, instResult, FALSE ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update install coord[%s:%s]'s "
                    "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
         }
         goto error ;
      }
      
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaInstDBBusTask::_installDataRG()
   {
      INT32 rc = SDB_OK ;
      INT32 threadNum = 0 ;
      INT32 dataGroupNum = _mapGroups.size() ;
      
      if ( 0 == dataGroupNum )
      {
         rc = SDB_INVALIDARG ;
         PD_LOG_MSG ( PDERROR, "No information for installing data group" ) ;
         goto error ;
      }
      threadNum = dataGroupNum < ADD_HOST_MAX_THREAD_NUM ? dataGroupNum :
                  ADD_HOST_MAX_THREAD_NUM ;
      for( INT32 i = 0; i < threadNum; i++ )
      { 
         ossScopedLock lock( &_taskLatch, EXCLUSIVE ) ;
         if ( TRUE == pmdGetThreadEDUCB()->isInterrupted() )
         {
            PD_LOG( PDEVENT, "Program has been interrupted, stop task[%s]",
                    _taskName.c_str() ) ;
            goto done ;
         }
         if ( OMA_TASK_STATUS_RUNNING == _taskStatus )
         {
            rc = startOmagentJob( OMA_TASK_INSTALL_DB_SUB, _taskID,
                                  BSONObj(), (void *)this ) ;
            if ( rc )
            {
               PD_LOG ( PDERROR, "Failed to run add host sub task with the "
                        "type[%d], rc = %d", OMA_TASK_INSTALL_DB_SUB, rc ) ;
               goto error ;
            }
         }
      }
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_rollback()
   {
      INT32 rc = SDB_OK ;

      setTaskStatus( OMA_TASK_STATUS_ROLLBACK ) ;
      rc = _updateProgressToOM() ;
      if ( rc )
      {
         rc = SDB_OK ;
         PD_LOG( PDWARNING, "Failed to update task's progress to om" ) ;
      }
      
      if ( TRUE == _isStandalone )
      {
         rc = _rollbackStandalone() ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to rollback standalone, rc = %d", rc ) ;
            goto error ;
         }
      }
      else
      {
         rc = _rollbackDataRG () ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to rollback data groups, rc = %d", rc ) ;
            goto error ;
         }
         rc = _rollbackCoord () ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to rollback coord group, rc = %d", rc ) ;
            goto error ;
         }
         rc = _rollbackCatalog () ;
         if ( rc )
         {
            PD_LOG ( PDERROR, "Failed to rollback catalog group, rc = %d", rc ) ;
            goto error ;
         }
      }

   done:
      _setResultToFail() ;
      
      return rc ;
   error:
      goto done ;

   }

   INT32 _omaInstDBBusTask::_rollbackStandalone()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      BOOLEAN needToRollback       = FALSE ;
      CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
      const CHAR *pDetail          = NULL ;
      const CHAR *pHostName        = NULL ;
      const CHAR *pSvcName         = NULL ;
      InstDBResult instResult ;
      BSONObjBuilder bob ;
      BSONArrayBuilder bab ;
      BSONObj bus ;
      BSONObj sys ;
      BSONObj retObj ;
      vector<InstDBBusInfo>::iterator it = _standalone.begin() ;
      
      for ( ; it != _standalone.end(); it++ )
      {
         if ( OMA_TASK_STATUS_INIT != it->_instResult._status )
         {
            bus = BSON( OMA_FIELD_UNINSTALLHOSTNAME << it->_instInfo._hostName <<
                        OMA_FIELD_UNINSTALLSVCNAME << it->_instInfo._svcName ) ;
            sys = BSON( OMA_FIELD_TASKID << _taskID ) ;
            instResult = it->_instResult ;
            needToRollback = TRUE ;
            break ;
         }
      }
      _omaRollbackStandalone runCmd ( bus, sys ) ;
      if ( FALSE == needToRollback )
      {
         PD_LOG ( PDEVENT, "No standalone need to rollback" ) ;
         goto done ;
      } 
      pHostName = it->_instInfo._hostName.c_str() ;
      pSvcName  = it->_instInfo._svcName.c_str() ;

      ossSnprintf( flow, OMA_BUFF_SIZE, "Rollbacking standalone[%s:%s]",
                   pHostName, pSvcName ) ;
      instResult._flow.push_back( flow ) ;
      rc = updateProgressToTask( it->_nodeSerialNum, instResult, FALSE ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to update progress before install "
                  "standalone, rc = %d", rc ) ;
         goto error ;
      }
      
      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to rollback standalone[%s:%s], "
                 "rc = %d", pHostName, pSvcName, rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to rollback standalone" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to rollback standalone[%s:%s], rc = %d",
                 pHostName, pSvcName, rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Rollback standalone does not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "rollback standalone[%s:%s], rc = %d",
                 pHostName, pSvcName, rc ) ;
         pDetail = "Failed to get errno from js after rollback standalone" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "rollback standalone[%s:%s], rc = %d",
                    pHostName, pSvcName, tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "rollback standalone" ;
         }
         goto error ;
      }
      else
      {
         ossSnprintf( flow, OMA_BUFF_SIZE,
                      "Finish rollback standalone[%s:%s]",
                      pHostName, pSvcName ) ;
         PD_LOG ( PDEVENT, "Success to rollback standalone[%s:%s]",
                  pHostName, pSvcName ) ;
         instResult._status     = OMA_TASK_STATUS_FINISH ;
         instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
         instResult._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( it->_nodeSerialNum, instResult, FALSE ) ;
         if ( tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update rollback standalone[%s:%s]'s "
                    "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
         }
      }

   done:
      return rc ;
   error:
      ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to rollback "
                   "standalone[%s:%s]", pHostName, pSvcName ) ;
      instResult._status     = OMA_TASK_STATUS_FINISH ;
      instResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
      instResult._errno      = rc ;
      instResult._detail     = pDetail ;
      instResult._flow.push_back( flow ) ;
      tmpRc = updateProgressToTask( it->_nodeSerialNum, instResult, FALSE ) ;
      if ( tmpRc )
      {
         PD_LOG( PDWARNING, "Failed to update rollback standalone[%s:%s]'s "
                 "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
      }
      
      goto done ; 
   }

   INT32 _omaInstDBBusTask::_rollbackCatalog()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = 0 ;
      const CHAR *pDetail           = "" ;
      BSONObj retObj ;
      _omaRollbackCatalog runCmd( _taskID, _tmpCoordSvcName ) ;

      updateProgressToTask( SDB_OK, "", ROLE_CATA, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to rollback catalog, "
                 "rc = %d", rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to rollback catalog" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to rollback catalog, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "rollback catalog, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after rollback catalog" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "rollback catalog, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "rollback catalog" ;
         }
         goto error ;
      }
      else
      {

         PD_LOG ( PDEVENT, "Success to rollback catalog group" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_CATA, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG_MSG( PDERROR, "Failed to rollback catalog: %s, rc = %d",
                  pDetail, rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_CATA, OMA_TASK_STATUS_END ) ;

      goto done ;
   }

   INT32 _omaInstDBBusTask::_rollbackCoord()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = 0 ;
      const CHAR *pDetail           = "" ;
      BSONObj retObj ;
      _omaRollbackCoord runCmd( _taskID, _tmpCoordSvcName ) ;

      updateProgressToTask( SDB_OK, "", ROLE_COORD, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to rollback coord, "
                 "rc = %d", rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to rollback coord" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to rollback coord, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "rollback coord, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after rollback coord" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "rollback coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "rollback coord" ;
         }
         goto error ;
      }
      else
      {
         PD_LOG ( PDEVENT, "Success to rollback coord group" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_COORD, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG_MSG( PDERROR, "Failed to rollback coord: %s, rc = %d",
                  pDetail, rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_COORD, OMA_TASK_STATUS_END ) ;

      goto done ;
   }

   INT32 _omaInstDBBusTask::_rollbackDataRG()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = SDB_OK ;
      const CHAR *pDetail           = NULL ;
      BSONObj retObj ;
      _omaRollbackDataRG runCmd ( _taskID, _tmpCoordSvcName, _existGroups ) ;

      updateProgressToTask( SDB_OK, "", ROLE_DATA, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to rollback data groups, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init to rollback data groups" ;
         }
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to rollback data groups, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }  
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "rollback data groups, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after rollback data groups" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "rollback data groups, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "rollback data groups" ;
         }
         goto error ;
      }
      else
      {
         PD_LOG ( PDEVENT, "Success to rollback data groups" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_DATA, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG_MSG( PDERROR, "Failed to rollback data gropus: %s, rc = %d",
                  pDetail, rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_DATA, OMA_TASK_STATUS_END ) ;

      goto done ;
   }

   void _omaInstDBBusTask::_buildResultInfo( BOOLEAN isStandalone,
                                             pair<string, string> &p,
                                             BSONArrayBuilder &bab )
   {
      BSONObjBuilder builder ;
      BSONArrayBuilder arrBuilder ;
      BSONObj obj ;
      BOOLEAN canBuild = FALSE ;
      vector<InstDBBusInfo>::iterator it ;
      map< string, vector<InstDBBusInfo> >::iterator it2 ;
      vector<string>::iterator itr ;

      if ( TRUE == isStandalone )
      {
         it = _standalone.begin() ;
         for ( ; it != _standalone.end(); it++ )
         {
            if ( p.first == it->_instResult._hostName &&
                 p.second == it->_instResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
      }
      else
      {
         it = _catalog.begin() ;
         for ( ; it != _catalog.end(); it++ )
         {
            if ( p.first == it->_instResult._hostName &&
                 p.second == it->_instResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
         it = _coord.begin() ;
         for ( ; it != _coord.end(); it++ )
         {
            if ( p.first == it->_instResult._hostName &&
                 p.second == it->_instResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
         it2 = _mapGroups.begin() ;
         for ( ; it2 != _mapGroups.end(); it2++ )
         {
            it = it2->second.begin() ;
            for ( ; it != it2->second.end(); it++ )
            {
               if ( p.first == it->_instResult._hostName &&
                    p.second == it->_instResult._svcName )
               {
                  canBuild = TRUE ;
                  goto build ;
               }
            }
         }
      }

   build:
      if ( TRUE == canBuild )
      {
         itr = it->_instResult._flow.begin() ;
         for ( ; itr != it->_instResult._flow.end(); itr++ )
            arrBuilder.append( *itr ) ;
         
         builder.append( OMA_FIELD_ERRNO, it->_instResult._errno ) ;
         builder.append( OMA_FIELD_DETAIL, it->_instResult._detail ) ;         
         builder.append( OMA_FIELD_HOSTNAME, it->_instResult._hostName ) ;
         builder.append( OMA_FIELD_SVCNAME, it->_instResult._svcName ) ;
         builder.append( OMA_FIELD_ROLE, it->_instResult._role ) ;
         builder.append( OMA_OPTION_DATAGROUPNAME, it->_instResult._groupName ) ;
         builder.append( OMA_FIELD_STATUS, it->_instResult._status ) ;
         builder.append( OMA_FIELD_STATUSDESC, it->_instResult._statusDesc ) ;
         builder.append( OMA_FIELD_FLOW, arrBuilder.arr() ) ;
         
         obj = builder.obj() ;
         bab.append( obj ) ;
      }
   }

   void _omaInstDBBusTask::_buildUpdateTaskObj( BSONObj &retObj )
   {
      
      BSONObjBuilder bob ;
      BSONArrayBuilder bab ;

      vector< pair<string, string> >::iterator it = _resultOrder.begin() ;
      for ( ; it != _resultOrder.end(); it++ )
      {
         _buildResultInfo( _isStandalone, *it, bab ) ;
      }

      bob.appendNumber( OMA_FIELD_TASKID, _taskID ) ;
      if ( OMA_TASK_STATUS_FINISH == _taskStatus )
      {
         bob.appendNumber( OMA_FIELD_ERRNO, _errno ) ;
         bob.append( OMA_FIELD_DETAIL, _detail ) ;
      }
      else
      {
         bob.appendNumber( OMA_FIELD_ERRNO, SDB_OK ) ;
         bob.append( OMA_FIELD_DETAIL, "" ) ;
      }
      bob.appendNumber( OMA_FIELD_STATUS, _taskStatus ) ;
      bob.append( OMA_FIELD_STATUSDESC, getTaskStatusDesc( _taskStatus ) ) ;
      bob.appendNumber( OMA_FIELD_PROGRESS, _progress ) ;
      bob.appendArray( OMA_FIELD_RESULTINFO, bab.arr() ) ;

      retObj = bob.obj() ;
   }

   INT32 _omaInstDBBusTask::_calculateProgress()
   {
      INT32 rc            = SDB_OK ;
      INT32 totalNum      = 0 ;
      INT32 finishNum     = 0 ;
      vector<InstDBBusInfo>::iterator it ;
      map< string, vector<InstDBBusInfo> >::iterator it2 ;

      if ( TRUE == _isStandalone )
      {
         totalNum = _standalone.size() ;
         if ( 0 == totalNum )
         {
            rc = SDB_SYS ;
            PD_LOG_MSG( PDERROR, "Install standalone's info is empty" ) ;
            goto error ;
         }
         it = _standalone.begin() ;
         for( ; it != _standalone.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_instResult._status )
               finishNum++ ;
         }
         _progress = ( finishNum * 100 ) / totalNum ;
      }
      else
      {
         totalNum = _catalog.size() + _coord.size() ;
         it2 = _mapGroups.begin() ;
         for ( ; it2 != _mapGroups.end(); it2++ )
            totalNum += it2->second.size() ;
         it = _catalog.begin() ;
         for( ; it != _catalog.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_instResult._status )
               finishNum++ ;
         }
         it = _coord.begin() ;
         for( ; it != _coord.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_instResult._status )
               finishNum++ ;
         }
         it2 = _mapGroups.begin() ;
         for ( ; it2 != _mapGroups.end(); it2++ )
         {
            it = it2->second.begin() ;
            for( ; it != it2->second.end(); it++ )
            {
               if ( OMA_TASK_STATUS_FINISH == it->_instResult._status )
                  finishNum++ ;
            }
         }
         _progress = ( finishNum * 100 ) / totalNum ;     
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_updateProgressToOM()
   {
      INT32 rc            = SDB_OK ;
      INT32 retRc         = SDB_OK ;
      UINT64 reqID        = 0 ;
      omAgentMgr *pOmaMgr = sdbGetOMAgentMgr() ;
      _pmdEDUCB *cb       = pmdGetThreadEDUCB () ;
      ossAutoEvent updateEvent ;
      BSONObj obj ;
      
      _buildUpdateTaskObj( obj ) ;

      reqID = pOmaMgr->getRequestID() ;
      pOmaMgr->registerTaskEvent( reqID, &updateEvent ) ;
      
      while( !cb->isInterrupted() )
      {
         pOmaMgr->sendUpdateTaskReq( reqID, &obj ) ;
         while ( !cb->isInterrupted() )
         {
            if ( SDB_OK != updateEvent.wait( OMA_WAIT_OMSVC_RES_TIMEOUT, &retRc ) )
            {
               break ;
            }
            else
            {
               if ( SDB_OM_TASK_NOT_EXIST == retRc )
               {
                  PD_LOG( PDERROR, "Failed to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  rc = retRc ;
                  goto error ;
               }
               else if ( SDB_OK != retRc )
               {
                  PD_LOG( PDWARNING, "Retry to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  break ;
               }
               else
               {
                  PD_LOG( PDDEBUG, "Success to update task[%s]'s progress "
                          "with requestID[%lld]", _taskName.c_str(), reqID ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  goto done ;
               }
            }
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when update install db business task "
              "progress to omsvc" ) ;
      rc = SDB_APP_INTERRUPT ;
      
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaInstDBBusTask::_waitAndUpdateProgress()
   {
      INT32 rc = SDB_OK ;
      BOOLEAN flag = FALSE ;
      UINT64 subTaskEventID = 0 ;
      _pmdEDUCB *cb = pmdGetThreadEDUCB () ;

      while ( !cb->isInterrupted() )
      {
         if ( SDB_OK != _taskEvent.wait ( OMA_WAIT_SUB_TASK_NOTIFY_TIMEOUT ) )
         {
            continue ;
         }
         else
         {
            while( TRUE )
            {
               _taskLatch.get() ;
               _taskEvent.reset() ;
               flag = ( subTaskEventID < _eventID ) ? TRUE : FALSE ;
               subTaskEventID = _eventID ;
               _taskLatch.release() ;
               if ( TRUE == flag )
               {
                  rc = _updateProgressToOM() ;
                  if ( SDB_APP_INTERRUPT == rc )
                  {
                     PD_LOG( PDERROR, "Failed to update add host progress"
                             " to omsvc, rc = %d", rc ) ;
                     goto error ;
                  }
                  else if ( SDB_OK != rc )
                  {
                     PD_LOG( PDERROR, "Failed to update add host progress"
                             " to omsvc, rc = %d", rc ) ;
                  }
               }
               else
               {
                  break ;
               }
            }
            if ( _isTaskFinish() )
            {
               PD_LOG( PDEVENT, "All the add host sub tasks had finished" ) ;
               goto done ;
            }
            
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when running add host task" ) ;
      rc = SDB_APP_INTERRUPT ;
    
   done:
      return rc ;
   error:
      goto done ; 
   }

   BOOLEAN _omaInstDBBusTask::_isTaskFinish()
   {
      INT32 runNum    = 0 ;
      INT32 rbNum     = 0 ;
      INT32 finishNum = 0 ;
      INT32 otherNum  = 0 ;
      BOOLEAN flag    = TRUE ;
      ossScopedLock lock( &_latch, EXCLUSIVE ) ;
      
      map< string, OMA_TASK_STATUS >::iterator it = _subTaskStatus.begin() ;
      for ( ; it != _subTaskStatus.end(); it++ )
      {
         switch ( it->second )
         {
         case OMA_TASK_STATUS_FINISH :
            finishNum++ ;
            break ;
         case OMA_TASK_STATUS_RUNNING :
            runNum++ ;
            flag = FALSE ;
            break ;
         case OMA_TASK_STATUS_ROLLBACK :
            rbNum++ ;
            flag = FALSE ;
         default :
            otherNum++ ;
            flag = FALSE ;
            break ;
         }
      }
      PD_LOG( PDDEBUG, "In task[%s], there are [%d] sub task(s): "
              "[%d]running, [%d]rollback,[%d]finish, [%d]in the other status",
              _taskName.c_str(), _subTaskStatus.size(),
              runNum, rbNum, finishNum, otherNum ) ;

      return flag ;
   }

   BOOLEAN _omaInstDBBusTask::_needToRollback()
   {
      vector<InstDBBusInfo>::iterator it ;
      map< string, vector<InstDBBusInfo> >::iterator itr ;
      
      ossScopedLock lock( &_taskLatch, EXCLUSIVE ) ;
      
      it = _catalog.begin() ;
      for ( ; it != _catalog.end(); it++ )
      {
         if ( OMA_TASK_STATUS_ROLLBACK == it->_instResult._status )
            return TRUE ;
      }

      it = _coord.begin() ;
      for ( ; it != _coord.end(); it++ )
      {
         if ( OMA_TASK_STATUS_ROLLBACK == it->_instResult._status )
            return TRUE ;
      }

      itr = _mapGroups.begin() ;
      for ( ; itr != _mapGroups.end(); itr++ )
      {
         it = itr->second.begin() ;
         for ( ; it != itr->second.end(); it++ )
         {
            if ( OMA_TASK_STATUS_ROLLBACK == it->_instResult._status )
               return TRUE ;
         }
      }
      return FALSE ;
   }

   void _omaInstDBBusTask::_setRetErr( INT32 errNum )
   {
      const CHAR *pDetail = NULL ;

      if ( SDB_OK != _errno && '\0' != _detail[0] )
      {
         return ;
      }
      else
      {
         _errno = errNum ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL != pDetail && 0 != *pDetail )
         {
            ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
         }
         else
         {
            pDetail = getErrDesp( errNum ) ;
            if ( NULL != pDetail )
               ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
            else
               PD_LOG( PDERROR, "Failed to get error message" ) ;
         }
      }
   }

   void _omaInstDBBusTask::_setResultToFail()
   {
      vector<InstDBBusInfo>::iterator it ;
      map< string, vector<InstDBBusInfo> >::iterator it2 ;
      
      if ( TRUE == _isStandalone )
      {
         it = _standalone.begin() ;
         for ( ; it != _standalone.end(); it++ )
         {
            if ( SDB_OK == it->_instResult._errno )
            {
               it->_instResult._errno = SDB_OMA_TASK_FAIL ;
               it->_instResult._detail = getErrDesp( SDB_OMA_TASK_FAIL ) ;
            }
         }
      }
      else
      {
         it = _catalog.begin() ;
         for ( ; it != _catalog.end(); it++ )
         {
            if ( SDB_OK == it->_instResult._errno )
            {
               it->_instResult._errno = SDB_OMA_TASK_FAIL ;
               it->_instResult._detail = getErrDesp( SDB_OMA_TASK_FAIL ) ;
            }
         }
         it = _coord.begin() ;
         for ( ; it != _coord.end(); it++ )
         {
            if ( SDB_OK == it->_instResult._errno )
            {
               it->_instResult._errno = SDB_OMA_TASK_FAIL ;
               it->_instResult._detail = getErrDesp( SDB_OMA_TASK_FAIL ) ;
            }
         }
         it2 = _mapGroups.begin() ;
         for ( ; it2 != _mapGroups.end(); it2++ )
         {
            it = it2->second.begin() ;
            for ( ; it != it2->second.end(); it++ )
            {
               if ( SDB_OK == it->_instResult._errno )
               {
                  it->_instResult._errno = SDB_OMA_TASK_FAIL ;
                  it->_instResult._detail = getErrDesp( SDB_OMA_TASK_FAIL ) ;
               }
            }
         }
      }
   }

   /*
      remove db business task
   */
   _omaRemoveDBBusTask::_omaRemoveDBBusTask( INT64 taskID )
   : _omaTask( taskID )
   {
      _taskType      = OMA_TASK_REMOVE_DB ;
      _taskName      = OMA_TASK_NAME_REMOVE_DB_BUSINESS ;
      _isStandalone  = FALSE ;
      _nodeSerialNum = 0 ;
      _progress      = 0 ;
      _errno         = SDB_OK ;
      ossMemset( _detail, 0, OMA_BUFF_SIZE + 1 ) ;
   }

   _omaRemoveDBBusTask::~_omaRemoveDBBusTask()
   {
   }

   INT32 _omaRemoveDBBusTask::init( const BSONObj &info, void *ptr )
   {
      INT32 rc = SDB_OK ;

      _removeDBBusRawInfo = info.copy() ;
      PD_LOG ( PDDEBUG, "Remove db business passes argument: %s",
               _removeDBBusRawInfo.toString( FALSE, TRUE ).c_str() ) ;

      rc = _initTaskInfo( _removeDBBusRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to get remove db business info "
                 "rc = %d", rc ) ;
         goto error ;
      }
      rc = _initResultOrder( _removeDBBusRawInfo ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init result order rc = %d", rc ) ;
         goto error ;
      }

      rc = initJsEnv() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to init environment for executing js script, "
                 "rc = %d", rc ) ;
      }

      done:
         return rc ;
      error:
         goto done ;
   }

   INT32 _omaRemoveDBBusTask::doit()
   {
      INT32 rc = SDB_OK ;

      setTaskStatus( OMA_TASK_STATUS_RUNNING ) ;

      if ( _isStandalone )
      {
         rc = _removeStandalone() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to remove standalone, rc = %d", rc ) ;
            goto error ;
         }
      }
      else // in case of cluster
      {
         rc = _installTmpCoord() ;
         if ( SDB_OK != rc )
         {
            PD_LOG ( PDERROR, "Failed to install temporary coord, "
                     "rc = %d", rc ) ;
            goto error ;
         }
         rc = _removeDataRG() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to remove data groups, "
                    "rc = %d", rc ) ;
            goto error ;
         }
         rc = _removeCoord() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to remove coord group, "
                    "rc = %d", rc ) ;
            goto error ;
         }
         rc = _removeCatalog() ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Failed to remove catalog group, "
                    "rc = %d", rc ) ;
            goto error ;
         }
      }
      
   done:
      setTaskStatus( OMA_TASK_STATUS_FINISH ) ;
      
      if ( FALSE == _isStandalone )
      {
         rc = _removeTmpCoord() ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "Failed to remove temporary coord, rc = %d", rc ) ;
         }
      }

      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Failed to update install db business progress"
                 "to omsvc, rc = %d", rc ) ;
      }
      
      sdbGetOMAgentMgr()->submitTaskInfo( _taskID ) ;
      
      PD_LOG( PDEVENT, "Omagent finish running remove db business "
              "task[%lld]", _taskID ) ;
      
      return SDB_OK ;
   error:
      _setRetErr( rc ) ;
      goto done ;
   }

   string _omaRemoveDBBusTask::getTmpCoordSvcName()
   {
      return _tmpCoordSvcName ;
   }

   INT32 _omaRemoveDBBusTask::updateProgressToTask( INT32 serialNum,
                                                    RemoveDBResult &result,
                                                    BOOLEAN needToNotify )
   {
      INT32 rc            = SDB_OK ;
      vector<RemoveDBBusInfo>::iterator it ;
      
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      PD_LOG( PDDEBUG, "Remove db business update progress to local "
              "task: serialNum[%d], hostName[%s], svcName[%s], role[%s], "
              "groupName[%s], status[%d], statusDesc[%s], errno[%d], "
              "detail[%s], flow num[%d]",
              serialNum, result._hostName.c_str(),
              result._svcName.c_str(), result._role.c_str(),
              result._groupName.c_str(), result._status,
              result._statusDesc.c_str(), result._errno,
              result._detail.c_str(), result._flow.size() ) ;
 
      if ( TRUE == _isStandalone )
      {
         it = _standalone.begin() ;
         for ( ; it != _standalone.end(); it++ )
         {
            if ( serialNum == it->_nodeSerialNum )
            {
               it->_removeResult = result ;
               break ;
            }
         }
      }
      else
      {
         if ( string(ROLE_DATA) == result._role )
         {
            it = _data.begin() ;
            for ( ; it != _data.end(); it++ )
            {
               if ( serialNum == it->_nodeSerialNum )
               {
                  it->_removeResult = result ;
                  break ;
               }
            }
         }
         else if ( string(ROLE_COORD) == result._role )
         {
            it = _coord.begin() ;
            for ( ; it != _coord.end(); it++ )
            {
               if ( serialNum == it->_nodeSerialNum )
               {
                  it->_removeResult = result ;
                  break ;
               }
            }
         }
         else if ( string(ROLE_CATA) == result._role )
         {
            it = _catalog.begin() ;
            for ( ; it != _catalog.end(); it++ )
            {
               if ( serialNum == it->_nodeSerialNum )
               {
                  it->_removeResult = result ;
                  break ;
               }
            }
         }
         else
         {
            rc = SDB_SYS ;
            PD_LOG( PDWARNING, "Unknown role for updating progress when "
                    "removing db business" ) ;
            goto error ;
         }
      }
      
      rc = _calculateProgress() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to calculate task's progress, "
                 "rc = %d", rc ) ;
      }

      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to update remove db business progress"
                 "to omsvc, rc = %d", rc ) ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::updateProgressToTask( INT32 errNum,
                                                    const CHAR *pDetail,
                                                    const CHAR *pRole,
                                                    OMA_TASK_STATUS status )
   {
      INT32 rc = SDB_OK ;
      string str ;
      string flow ;
      stringstream ss ;
      vector<RemoveDBBusInfo>::iterator it ;
#define BEGIN_REMOVE_GROUP  "Removing "
#define FINISH_REMOVE_GROUP "Finish remving "
#define FAIL_REMOVE_GROUP   "Failed to remove "

      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      switch( status )
      {
      case OMA_TASK_STATUS_INIT:
      case OMA_TASK_STATUS_RUNNING:
         str = BEGIN_REMOVE_GROUP ;
         break ;
      case OMA_TASK_STATUS_FINISH:
         str = FINISH_REMOVE_GROUP ;
         break ;
      default:
         str = FAIL_REMOVE_GROUP ;
         status = OMA_TASK_STATUS_FINISH ;
         break ;
      }
      if ( 0 == ossStrncmp( pRole, ROLE_DATA, sizeof(ROLE_DATA) ) )
      {
         it = _data.begin() ;
         for ( ; it != _data.end(); it++ )
         {
            it->_removeResult._errno = errNum ;
            it->_removeResult._detail = pDetail ;
            it->_removeResult._status = status ;
            it->_removeResult._statusDesc = getTaskStatusDesc( status ) ;
            flow.clear() ;
            ss.str("") ;
            ss << str << "data node [" << it->_removeInfo._hostName << ":" <<
               it->_removeInfo._svcName << "]" ;
            flow = ss.str() ;
            it->_removeResult._flow.push_back( flow ) ;
         }
      }
      else if ( 0 == ossStrncmp( pRole, ROLE_COORD, sizeof(ROLE_COORD) ) )
      {
         it = _coord.begin() ;
         for ( ; it != _coord.end(); it++ )
         {
            it->_removeResult._errno = errNum ;
            it->_removeResult._detail = pDetail ;
            it->_removeResult._status = status ;
            it->_removeResult._statusDesc = getTaskStatusDesc( status ) ;
            flow.clear() ;
            ss.str("") ;
            ss << str << "coord node [" << it->_removeInfo._hostName << ":" <<
               it->_removeInfo._svcName << "]" ;
            flow = ss.str() ;
            it->_removeResult._flow.push_back( flow ) ;
         }
      }
      else if ( 0 == ossStrncmp( pRole, ROLE_CATA, sizeof(ROLE_CATA) ) )
      {
         it = _catalog.begin() ;
         for ( ; it != _catalog.end(); it++ )
         {
            it->_removeResult._errno = errNum ;
            it->_removeResult._detail = pDetail ;
            it->_removeResult._status = status ;
            it->_removeResult._statusDesc = getTaskStatusDesc( status ) ;
            flow.clear() ;
            ss.str("") ;
            ss << str << "catalog node [" << it->_removeInfo._hostName << ":" <<
               it->_removeInfo._svcName << "]" ;
            flow = ss.str() ;
            it->_removeResult._flow.push_back( flow ) ;
         }
      }
      rc = _calculateProgress() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to calculate task's progress, "
                 "rc = %d", rc ) ;
      }
      
      rc = _updateProgressToOM() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Failed to update progress to omsvc, rc = %d", rc ) ;
      }

      return SDB_OK ;
   }

   void _omaRemoveDBBusTask::setErrInfo( INT32 errNum, const CHAR *pDetail )
   {
      ossScopedLock lock ( &_taskLatch, EXCLUSIVE ) ;

      if ( NULL == pDetail )
      {
         PD_LOG( PDWARNING, "Error detail is NULL" ) ;
         return ;
      }
      if ( ( SDB_OK == errNum ) ||
           ( SDB_OK != _errno && '\0' != _detail[0] ) )
         return ;
      else
      {
         _errno = errNum ;
         ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
      }
   }
   
   INT32 _omaRemoveDBBusTask::_initTaskInfo( BSONObj &info )
   {
      INT32 rc = SDB_OK ;
      BSONElement ele ;
      BSONObj hostInfoObj ;
      BSONObj filter ;
      BSONObjBuilder builder ;
      BSONObjBuilder builder2 ;
      BSONArrayBuilder bab ;
      string deplayMod ;
      const CHAR *pStr = NULL ;
      

      ele = info.getField( OMA_FIELD_TASKID ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task id from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      _taskID = ele.numberLong() ;
      ele = info.getField( OMA_FIELD_STATUS ) ;
      if ( NumberInt != ele.type() && NumberLong != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid task status from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      _taskStatus = (OMA_TASK_STATUS)ele.numberInt() ;

      rc = omaGetObjElement( info, OMA_FIELD_INFO, hostInfoObj ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_INFO, rc ) ;
      
      ele = hostInfoObj.getField( OMA_FIELD_DEPLOYMOD ) ;
      if ( String != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid content from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      deplayMod = ele.String() ;
      if ( deplayMod == string(DEPLAY_SA) )
      {
         _isStandalone = TRUE ;
      }
      else if ( deplayMod == string(DEPLAY_DB) )
      {
         _isStandalone = FALSE ;
      }
      else
      {
         PD_LOG_MSG ( PDERROR, "Receive invalid deplay mode from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_CLUSTERNAME, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_CLUSTERNAME, rc ) ;
      builder.append( OMA_FIELD_CLUSTERNAME2, pStr ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_BUSINESSNAME, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_BUSINESSNAME, rc ) ;
      builder.append( OMA_FIELD_BUSINESSNAME2, pStr ) ;      
      builder.append( OMA_FIELD_USERTAG2, OMA_TMP_COORD_NAME ) ;

      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_AUTHUSER, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_AUTHUSER, rc ) ;
      builder2.append( OMA_FIELD_AUTHUSER, pStr ) ;
      rc = omaGetStringElement ( hostInfoObj, OMA_FIELD_AUTHPASSWD, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                "rc: %d", OMA_FIELD_AUTHPASSWD, rc ) ;
      builder2.append( OMA_FIELD_AUTHPASSWD, pStr ) ;
      _authInfo = builder2.obj() ;

      ele = hostInfoObj.getField ( OMA_FIELD_CONFIG ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format install "
                      "db business info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         while ( itr.more() )
         {
            RemoveDBBusInfo removeDBBusInfo ;
            BSONObjBuilder bob ;
            BSONObj hostInfo ;
            BSONObj temp ;
            const CHAR *pRole = NULL ;
            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            temp = ele.embeddedObject() ;
            bob.appendElements( temp ) ;
            bob.appendElements( _authInfo ) ;
            hostInfo = bob.obj() ;
            rc = omaGetStringElement ( hostInfo, OMA_OPTION_ROLE, &pRole ) ;
            if ( rc )
            {
               PD_LOG_MSG ( PDERROR, "Get field[%s] failed, rc = %d",
                            OMA_OPTION_ROLE, rc ) ;
               goto error ;
            }
            if ( 0 == ossStrncmp( pRole, ROLE_DATA,
                                  ossStrlen( ROLE_DATA ) ) )
            {
               rc = _initRemoveAndResultInfo( hostInfo, removeDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init remove db business info and result, "
                         "rc: %d", rc ) ;
               _data.push_back( removeDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_COORD,
                                       ossStrlen( ROLE_COORD ) ) )
            {
               rc = _initRemoveAndResultInfo( hostInfo, removeDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init remove db business info and result, "
                         "rc: %d", rc ) ;
               _coord.push_back( removeDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_CATA,
                                       ossStrlen( ROLE_CATA ) ) )
            {
               BSONObjBuilder bob ;
               rc = omaGetStringElement ( hostInfo, OMA_FIELD_HOSTNAME, &pStr ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                         "rc: %d", OMA_FIELD_HOSTNAME, rc ) ;
               bob.append( OMA_FIELD_HOSTNAME, pStr ) ;
               rc = omaGetStringElement ( hostInfo, OMA_OPTION_CATANAME, &pStr ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR, "Get field[%s] failed, "
                         "rc: %d", OMA_OPTION_CATANAME, rc ) ;
               bob.append( OMA_FIELD_SVCNAME2, pStr ) ;
               bab.append( bob.obj() ) ;
               rc = _initRemoveAndResultInfo( hostInfo, removeDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init remove db business info and result, "
                         "rc: %d", rc ) ;
               _catalog.push_back( removeDBBusInfo ) ;
            }
            else if ( 0 == ossStrncmp( pRole, ROLE_STANDALONE,
                                       ossStrlen( ROLE_STANDALONE ) ) )
            {
               rc = _initRemoveAndResultInfo( hostInfo, removeDBBusInfo ) ;
               PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                         "Failed to init remove db business info and result, "
                         "rc: %d", rc ) ;
               _standalone.push_back( removeDBBusInfo ) ;
            }
            else
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG( PDERROR, "Unknown role for remove db business" ) ;
               goto error ;
            }
         } // while
         builder.appendArray( OMA_FIELD_CATAADDR, bab.arr() ) ;
         _tmpCoordCfgObj = builder.obj() ;
      }

   done:
      return rc ;
   error :
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_initRemoveAndResultInfo( BSONObj& hostInfo,
                                                        RemoveDBBusInfo &info )
   { 
      INT32 rc               = SDB_OK ; 
      const CHAR *pHostName  = NULL ;
      const CHAR *pSvcName   = NULL ;
      const CHAR *pGroupName = NULL ;
      const CHAR *pRole      = NULL ;
      const CHAR *pStr       = NULL ;

      info._nodeSerialNum = _nodeSerialNum++ ;
      
      rc = omaGetStringElement( hostInfo, OMA_FIELD_HOSTNAME, &pHostName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_HOSTNAME, rc ) ;
      info._removeInfo._hostName = pHostName ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_SVCNAME, &pSvcName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_OPTION_SVCNAME, rc ) ;
      info._removeInfo._svcName = pSvcName ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_ROLE, &pRole ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_OPTION_ROLE, rc ) ;
      info._removeInfo._role = pRole ;
      rc = omaGetStringElement( hostInfo, OMA_OPTION_DATAGROUPNAME,
                                &pGroupName ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_OPTION_DATAGROUPNAME, rc ) ;
      info._removeInfo._dataGroupName = pGroupName ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_AUTHUSER, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d",
                OMA_FIELD_AUTHUSER, rc ) ;
      info._removeInfo._authUser = pStr ;
      rc = omaGetStringElement( hostInfo, OMA_FIELD_AUTHPASSWD, &pStr ) ;
      PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                "Get field[%s] failed, rc: %d", OMA_FIELD_AUTHPASSWD, rc ) ;
      info._removeInfo._authPasswd = pStr ;
      
      info._removeResult._errno      = SDB_OK ;
      info._removeResult._detail     = "" ;
      info._removeResult._hostName   = pHostName ;
      info._removeResult._svcName    = pSvcName ;
      info._removeResult._role       = pRole ;
      info._removeResult._groupName  = pGroupName ;
      info._removeResult._status     = OMA_TASK_STATUS_INIT ;
      info._removeResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_INIT ) ;

   done:
      return rc ;
   error:
      goto done ;

   }

   INT32 _omaRemoveDBBusTask::_initResultOrder( BSONObj &info )
   {
      INT32 rc = SDB_OK ;
      BSONElement ele ;
      const CHAR *pHostName = NULL ;
      const CHAR *pSvcName  = NULL ;

      ele = info.getField ( OMA_FIELD_RESULTINFO ) ;
      if ( Array != ele.type() )
      {
         PD_LOG_MSG ( PDERROR, "Receive wrong format remove "
                      "db business info from omsvc" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      else
      {
         BSONObjIterator itr( ele.embeddedObject() ) ;
         
         while ( itr.more() )
         {
            BSONObj resultInfo ;
            pair<string, string> p ;
            ele = itr.next() ;
            if ( Object != ele.type() )
            {
               rc = SDB_INVALIDARG ;
               PD_LOG_MSG ( PDERROR, "Receive wrong format bson from omsvc" ) ;
               goto error ;
            }
            resultInfo = ele.embeddedObject() ;
            rc = omaGetStringElement( resultInfo, OMA_FIELD_HOSTNAME, &pHostName ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d", OMA_FIELD_HOSTNAME, rc ) ;
            rc = omaGetStringElement( resultInfo, OMA_FIELD_SVCNAME, &pSvcName ) ;
            PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                      "Get field[%s] failed, rc: %d", OMA_FIELD_SVCNAME, rc ) ;
            p.first = pHostName ;
            p.second = pSvcName ;
            _resultOrder.push_back( p ) ;
         }
      }
      
   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaRemoveDBBusTask::_getInfoToRemove( BSONObj &obj )
   {
      BSONObjBuilder builder ;
      BSONArrayBuilder bab ;
      set<string> groupNames ;
      vector<RemoveDBBusInfo>::iterator it ;
      set<string>::iterator it2 ;

      builder.appendElements( _authInfo ) ;
      
      it = _data.begin() ;
      for ( ; it != _data.end(); it++ )
      {
         groupNames.insert( it->_removeInfo._dataGroupName ) ;
      }
      it2 = groupNames.begin() ;
      for ( ; it2 != groupNames.end(); it2++ )
      {
         bab.append( *it2 ) ;
      }
      builder.appendArray( OMA_FIELD_UNINSTALLGROUPNAMES, bab.arr() ) ;
      obj = builder.obj() ;
   }

   INT32 _omaRemoveDBBusTask::_saveTmpCoordInfo( BSONObj &info )
   {
      INT32 rc         = SDB_OK ;
      const CHAR *pStr = NULL ;
      rc = omaGetStringElement( info, OMA_FIELD_TMPCOORDSVCNAME, &pStr ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to get filed[%s], rc = %s",
                  OMA_FIELD_TMPCOORDSVCNAME, rc ) ;
         goto error ;
      }
      _tmpCoordSvcName = pStr ;
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_installTmpCoord()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      const CHAR *pDetail          = NULL ;
      BSONObj retObj ;
      _omaCreateTmpCoord runCmd( _taskID ) ;
      
      rc = runCmd.createTmpCoord( _tmpCoordCfgObj, retObj ) ;
      if ( rc )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Install temporary coord does not execute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "installing temporay coord, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after "
                   "installing temporay coord" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "installing temporay coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "installing temporay coord" ;
            goto error ;
         }
         rc = errNum ;
         goto error ;
      }
      rc = _saveTmpCoordInfo( retObj ) ;
      if ( rc )
      {
         PD_LOG_MSG ( PDERROR, "Failed to save installed temporary coord's "
                      "info, rc = %d", rc ) ;
         pDetail = "Failed to save installed temporary coord's info" ;
         goto error ;
      }
      
   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_removeTmpCoord()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      const CHAR *pDetail          = NULL ;
      BSONObj retObj ;
      _omaRemoveTmpCoord runCmd( _taskID, _tmpCoordSvcName ) ;
      
      rc = runCmd.removeTmpCoord ( retObj ) ;
      if ( rc )
      {
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Remove temporary coord does not execute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG_MSG( PDERROR, "Failed to get errno from js after "
                     "removing temporay coord, rc = %d", rc ) ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "removing temporay coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "removing temporay coord" ;
            goto error ;
         }
         rc = errNum ;
         goto error ;
      }

   done:
      return rc ;
   error:
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_removeStandalone()
   {
      INT32 rc                     = SDB_OK ;
      INT32 tmpRc                  = SDB_OK ;
      INT32 errNum                 = 0 ;
      CHAR flow[OMA_BUFF_SIZE + 1] = { 0 } ;
      const CHAR *pDetail          = NULL ;
      const CHAR *pHostName        = NULL ;
      const CHAR *pSvcName         = NULL ;
      RemoveDBResult removeResult  = { SDB_OK, "", "", "", "", "",
                                       OMA_TASK_STATUS_RUNNING,
                                       OMA_TASK_STATUS_DESC_RUNNING } ;
      BSONObj bus ;
      BSONObj sys ;
      BSONObj retObj ;
      vector<RemoveDBBusInfo>::iterator it = _standalone.begin() ;
      if ( it == _standalone.end() )
      {
         PD_LOG( PDWARNING, "No standalone needs to be removed" ) ;
         goto done ;
      }
      
      {
      bus = BSON( OMA_FIELD_UNINSTALLHOSTNAME << it->_removeInfo._hostName <<
                  OMA_FIELD_UNINSTALLSVCNAME << it->_removeInfo._svcName ) ;
      sys = BSON( OMA_FIELD_TASKID << _taskID ) ;
      _omaRmStandalone runCmd ( bus, sys ) ;

      pHostName = it->_removeInfo._hostName.c_str() ;
      pSvcName  = it->_removeInfo._svcName.c_str() ;
      removeResult._hostName  = pHostName ;
      removeResult._svcName   = pSvcName ;
      removeResult._role      = it->_removeInfo._role ;
      removeResult._groupName = it->_removeInfo._dataGroupName ;

      ossSnprintf( flow, OMA_BUFF_SIZE, "Removing standalone[%s:%s]",
                   pHostName, pSvcName ) ;
      removeResult._flow.push_back( flow ) ;
      rc = updateProgressToTask( it->_nodeSerialNum, removeResult, FALSE ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to update progress before remove "
                  "standalone, rc = %d", rc ) ;
         goto error ;
      }
      
      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to remove standalone[%s:%s], "
                 "rc = %d", pHostName, pSvcName, rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to remove standalone" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to remove standalone[%s:%s], rc = %d",
                 pHostName, pSvcName, rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "remove standalone[%s:%s], rc = %d",
                 pHostName, pSvcName, rc ) ;
         pDetail = "Failed to get errno from js after remove standalone" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "remove standalone[%s:%s], rc = %d",
                    pHostName, pSvcName, tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "remove standalone" ;
         }
         goto error ;
      }
      else
      {
         ossSnprintf( flow, OMA_BUFF_SIZE,
                      "Finish remove standalone[%s:%s]",
                      pHostName, pSvcName ) ;
         PD_LOG ( PDEVENT, "Success to remove standalone[%s:%s]",
                  pHostName, pSvcName ) ;
         removeResult._status     = OMA_TASK_STATUS_FINISH ;
         removeResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
         removeResult._flow.push_back( flow ) ;
         tmpRc = updateProgressToTask( it->_nodeSerialNum, removeResult, FALSE ) ;
         if ( tmpRc )
         {
            PD_LOG( PDWARNING, "Failed to update remove standalone[%s:%s]'s "
                    "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
         }
      }
      }

   done:
      return rc ;
   error:
      ossSnprintf( flow, OMA_BUFF_SIZE, "Failed to remove "
                   "standalone[%s:%s]", pHostName, pSvcName ) ;
      removeResult._status     = OMA_TASK_STATUS_FINISH ;
      removeResult._statusDesc = getTaskStatusDesc( OMA_TASK_STATUS_FINISH ) ;
      removeResult._errno      = rc ;
      removeResult._detail     = pDetail ;
      removeResult._flow.push_back( flow ) ;
      tmpRc = updateProgressToTask( it->_nodeSerialNum, removeResult, FALSE ) ;
      if ( tmpRc )
      {
         PD_LOG( PDWARNING, "Failed to update remove standalone[%s:%s]'s "
                 "progress, rc = %d", pHostName, pSvcName, tmpRc ) ;
      }
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_removeCatalog()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = 0 ;
      const CHAR *pDetail           = "" ;
      BSONObj retObj ;
      _omaRmCataRG runCmd( _taskID, _tmpCoordSvcName, _authInfo ) ;

      updateProgressToTask( SDB_OK, "", ROLE_CATA, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to remove catalog, "
                 "rc = %d", rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to remove catalog" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to remove catalog, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "remove catalog, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after remove catalog" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "remove catalog, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "remove catalog" ;
         }
         goto error ;
      }
      else
      {
         PD_LOG ( PDEVENT, "Success to remove catalog group" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_CATA, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG_MSG( PDERROR, "Failed to remove catalog: rc = %d", rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_CATA, OMA_TASK_STATUS_END ) ;
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_removeCoord()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = 0 ;
      const CHAR *pDetail           = "" ;
      BSONObj retObj ;
      _omaRmCoordRG runCmd( _taskID, _tmpCoordSvcName, _authInfo ) ;

      updateProgressToTask( SDB_OK, "",ROLE_COORD, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to remove coord, "
                 "rc = %d", rc ) ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL == pDetail || 0 == *pDetail )
            pDetail = "Failed to init to remove coord" ;
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to remove coord, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "remove coord, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after remove coord" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "remove coord, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "remove coord" ;
         }
         goto error ;
      }
      else
      {
         PD_LOG ( PDEVENT, "Success to remove coord group" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_COORD, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG( PDERROR, "Failed to remove coord: rc = %d", rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_COORD, OMA_TASK_STATUS_END ) ;
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_removeDataRG()
   {
      INT32 rc                      = SDB_OK ;
      INT32 tmpRc                   = SDB_OK ;
      INT32 errNum                  = SDB_OK ;
      const CHAR *pDetail           = NULL ;
      BSONObj removeGroupObj ;
      BSONObj retObj ;

      _getInfoToRemove( removeGroupObj ) ;
      _omaRmDataRG runCmd ( _taskID, _tmpCoordSvcName, removeGroupObj ) ;

      updateProgressToTask( SDB_OK, "", ROLE_DATA, OMA_TASK_STATUS_RUNNING ) ;

      rc = runCmd.init( NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init to remove data groups, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Failed to init to remove data groups" ;
         }
         goto error ;
      }
      rc = runCmd.doit( retObj ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to remove data groups, rc = %d", rc ) ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( tmpRc )
         {
            pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
            if ( NULL == pDetail || 0 == *pDetail )
               pDetail = "Not exeute js file yet" ;
         }  
         goto error ;
      }
      rc = omaGetIntElement ( retObj, OMA_FIELD_ERRNO, errNum ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to get errno from js after "
                 "remove data groups, rc = %d", rc ) ;
         pDetail = "Failed to get errno from js after remove data groups" ;
         goto error ;
      }
      if ( SDB_OK != errNum )
      {
         rc = errNum ;
         tmpRc = omaGetStringElement ( retObj, OMA_FIELD_DETAIL, &pDetail ) ;
         if ( SDB_OK != tmpRc )
         {
            PD_LOG( PDERROR, "Failed to get error detail from js after "
                    "remove data groups, rc = %d", tmpRc ) ;
            pDetail = "Failed to get error detail from js after "
                      "remove data groups" ;
         }
         goto error ;
      }
      else
      {
         PD_LOG ( PDEVENT, "Success to remove data groups" ) ;
      }

      updateProgressToTask( SDB_OK, "", ROLE_DATA, OMA_TASK_STATUS_FINISH ) ;

   done:
      return rc ;
   error:
      PD_LOG( PDERROR, "Failed to remove data groups, rc = %d", rc ) ;
      updateProgressToTask( rc, pDetail, ROLE_DATA, OMA_TASK_STATUS_END ) ;
      setErrInfo( rc, pDetail ) ;
      goto done ;
   }

   void _omaRemoveDBBusTask::_buildResultInfo( BOOLEAN isStandalone,
                                               pair<string, string> &p,
                                               BSONArrayBuilder &bab )
   {
      BSONObjBuilder builder ;
      BSONArrayBuilder arrBuilder ;
      BSONObj obj ;
      BOOLEAN canBuild = FALSE ;
      vector<RemoveDBBusInfo>::iterator it ;
      vector<string>::iterator itr ;

      if ( TRUE == isStandalone )
      {
         it = _standalone.begin() ;
         for ( ; it != _standalone.end(); it++ )
         {
            if ( p.first == it->_removeResult._hostName &&
                 p.second == it->_removeResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
      }
      else
      {
         it = _catalog.begin() ;
         for ( ; it != _catalog.end(); it++ )
         {
            if ( p.first == it->_removeResult._hostName &&
                 p.second == it->_removeResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
         it = _coord.begin() ;
         for ( ; it != _coord.end(); it++ )
         {
            if ( p.first == it->_removeResult._hostName &&
                 p.second == it->_removeResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
         it = _data.begin() ;
         for ( ; it != _data.end(); it++ )
         {
            if ( p.first == it->_removeResult._hostName &&
                 p.second == it->_removeResult._svcName )
            {
               canBuild = TRUE ;
               goto build ;
            }
         }
      }

   build:
      if ( TRUE == canBuild )
      {
         itr = it->_removeResult._flow.begin() ;
         for ( ; itr != it->_removeResult._flow.end(); itr++ )
            arrBuilder.append( *itr ) ;
         
         builder.append( OMA_FIELD_ERRNO, it->_removeResult._errno ) ;
         builder.append( OMA_FIELD_DETAIL, it->_removeResult._detail ) ;         
         builder.append( OMA_FIELD_HOSTNAME, it->_removeResult._hostName ) ;
         builder.append( OMA_FIELD_SVCNAME, it->_removeResult._svcName ) ;
         builder.append( OMA_FIELD_ROLE, it->_removeResult._role ) ;
         builder.append( OMA_OPTION_DATAGROUPNAME, it->_removeResult._groupName ) ;
         builder.append( OMA_FIELD_STATUS, it->_removeResult._status ) ;
         builder.append( OMA_FIELD_STATUSDESC, it->_removeResult._statusDesc ) ;
         builder.append( OMA_FIELD_FLOW, arrBuilder.arr() ) ;
         
         obj = builder.obj() ;
         bab.append( obj ) ;
      }
   }

   void _omaRemoveDBBusTask::_buildUpdateTaskObj( BSONObj &retObj )
   {
      
      BSONObjBuilder bob ;
      BSONArrayBuilder bab ;

      vector< pair<string, string> >::iterator it = _resultOrder.begin() ;
      for ( ; it != _resultOrder.end(); it++ )
      {
         _buildResultInfo( _isStandalone, *it, bab ) ;
      }

      bob.appendNumber( OMA_FIELD_TASKID, _taskID ) ;
      if ( OMA_TASK_STATUS_FINISH == _taskStatus )
      {
         bob.appendNumber( OMA_FIELD_ERRNO, _errno ) ;
         bob.append( OMA_FIELD_DETAIL, _detail ) ;
      }
      else
      {
         bob.appendNumber( OMA_FIELD_ERRNO, SDB_OK ) ;
         bob.append( OMA_FIELD_DETAIL, "" ) ;
      }
      bob.appendNumber( OMA_FIELD_STATUS, _taskStatus ) ;
      bob.append( OMA_FIELD_STATUSDESC, getTaskStatusDesc( _taskStatus ) ) ;
      bob.appendNumber( OMA_FIELD_PROGRESS, _progress ) ;
      bob.appendArray( OMA_FIELD_RESULTINFO, bab.arr() ) ;

      retObj = bob.obj() ;
   }
   
   INT32 _omaRemoveDBBusTask::_calculateProgress()
   {
      INT32 rc        = SDB_OK ;
      INT32 totalNum  = 0 ;
      INT32 finishNum = 0 ;
      vector<RemoveDBBusInfo>::iterator it ;

      if ( TRUE == _isStandalone )
      {
         totalNum = _standalone.size() ;
         if ( 0 == totalNum )
         {
            rc = SDB_SYS ;
            PD_LOG_MSG( PDERROR, "Remove standalone's info is empty" ) ;
            goto error ;
         }
         it = _standalone.begin() ;
         for( ; it != _standalone.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_removeResult._status )
               finishNum++ ;
         }
         _progress = ( finishNum * 100 ) / totalNum ;
      }
      else
      {
         totalNum = _catalog.size() + _coord.size() + _data.size() ;
         
         it = _catalog.begin() ;
         for( ; it != _catalog.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_removeResult._status )
               finishNum++ ;
         }
         it = _coord.begin() ;
         for( ; it != _coord.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_removeResult._status )
               finishNum++ ;
         }
         it = _data.begin() ;
         for( ; it != _data.end(); it++ )
         {
            if ( OMA_TASK_STATUS_FINISH == it->_removeResult._status )
               finishNum++ ;
         }
         _progress = ( finishNum * 100 ) / totalNum ;         
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omaRemoveDBBusTask::_updateProgressToOM()
   {
      INT32 rc            = SDB_OK ;
      INT32 retRc         = SDB_OK ;
      UINT64 reqID        = 0 ;
      omAgentMgr *pOmaMgr = sdbGetOMAgentMgr() ;
      _pmdEDUCB *cb       = pmdGetThreadEDUCB () ;
      ossAutoEvent updateEvent ;
      BSONObj obj ;
      
      _buildUpdateTaskObj( obj ) ;

      reqID = pOmaMgr->getRequestID() ;
      pOmaMgr->registerTaskEvent( reqID, &updateEvent ) ;
      
      while( !cb->isInterrupted() )
      {
         pOmaMgr->sendUpdateTaskReq( reqID, &obj ) ;
         while ( !cb->isInterrupted() )
         {
            if ( SDB_OK != updateEvent.wait( OMA_WAIT_OMSVC_RES_TIMEOUT, &retRc ) )
            {
               break ;
            }
            else
            {
               if ( SDB_OM_TASK_NOT_EXIST == retRc )
               {
                  PD_LOG( PDERROR, "Failed to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  rc = retRc ;
                  goto error ;
               }
               else if ( SDB_OK != retRc )
               {
                  PD_LOG( PDWARNING, "Retry to update task[%s]'s progress "
                          "with requestID[%lld], rc = %d",
                          _taskName.c_str(), reqID, retRc ) ;
                  break ;
               }
               else
               {
                  PD_LOG( PDDEBUG, "Success to update task[%s]'s progress "
                          "with requestID[%lld]", _taskName.c_str(), reqID ) ;
                  pOmaMgr->unregisterTaskEvent( reqID ) ;
                  goto done ;
               }
            }
         }
      }

      PD_LOG( PDERROR, "Receive interrupt when update remove db business task "
              "progress to omsvc" ) ;
      rc = SDB_APP_INTERRUPT ;
      
   done:
      return rc ;
   error:
      goto done ;
   }

   void _omaRemoveDBBusTask::_setRetErr( INT32 errNum )
   {
      const CHAR *pDetail = NULL ;

      if ( SDB_OK != _errno && '\0' != _detail[0] )
      {
         return ;
      }
      else
      {
         _errno = errNum ;
         pDetail = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         if ( NULL != pDetail && 0 != *pDetail )
         {
            ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
         }
         else
         {
            pDetail = getErrDesp( errNum ) ;
            if ( NULL != pDetail )
               ossStrncpy( _detail, pDetail, OMA_BUFF_SIZE ) ;
            else
               PD_LOG( PDERROR, "Failed to get error message" ) ;
         }
      }
   }

} // namespace engine
