/*
 Copyright (c) 2013, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
*/


#include <NdbApi.hpp>

#include "adapter_global.h"
#include "js_wrapper_macros.h"
#include "Record.h"
#include "NativeMethodCall.h"
#include "NdbWrapperErrors.h"

using namespace v8;

/* Methods from ScanNextResultImpl.cpp */
typedef Handle<Value> __Method__(const Arguments &);
extern __Method__ scanNextResult;
extern __Method__ scanFetchResults;

/* Implemented here */
__Method__ NdbScanOperation_close;


/******** NdbScanOperation ******************************/

/* NdbOperation * lockCurrentTuple(NdbTransaction* lockTrans)
   IMMEDIATE
   
   FIXME: Returns an NdbOperation * which is no longer something we wrap for JS
*/
Handle<Value> lockCurrentTuple(const Arguments & args) {
  DEBUG_MARKER(UDEB_DETAIL);
  HandleScope scope;
  
  REQUIRE_ARGS_LENGTH(1);
  typedef NativeMethodCall_1_<NdbOperation *, NdbScanOperation, NdbTransaction *> NCALL;
  NCALL ncall(& NdbScanOperation::lockCurrentTuple, args);
  // ncall.wrapReturnValueAs(& NdbOperationEnvelope);
  ncall.run();
  
  // return scope.Close(ncall.jsReturnVal());
  return Null();
}


class NdbScanOperationEnvelopeClass : public Envelope {
public: 
  NdbScanOperationEnvelopeClass() : Envelope("NdbScanOperation") {
    DEFINE_JS_FUNCTION(Envelope::stencil, "getNdbError", 
                       getNdbError<NdbScanOperation>);
    DEFINE_JS_FUNCTION(Envelope::stencil, "fetchResults", scanFetchResults);
    DEFINE_JS_FUNCTION(Envelope::stencil, "nextResult", scanNextResult);
    DEFINE_JS_FUNCTION(Envelope::stencil, "lockCurrentTuple", lockCurrentTuple);
    DEFINE_JS_FUNCTION(Envelope::stencil, "close", NdbScanOperation_close);  
  }
};

NdbScanOperationEnvelopeClass NdbScanOperationEnvelope;

Envelope * getNdbScanOperationEnvelope() {
  return & NdbScanOperationEnvelope;
}


Handle<Value> NdbScanOperation_Wrapper(NdbScanOperation *op) {
  HandleScope scope;
  Local<Object> jsobj = NdbScanOperationEnvelope.newWrapper();
  wrapPointerInObject(op, NdbScanOperationEnvelope, jsobj);
  return scope.Close(jsobj);
}


Handle<Value> NdbScanOperation_close(const Arguments & args) {
  REQUIRE_ARGS_LENGTH(3);
  typedef NativeVoidMethodCall_2_<NdbScanOperation, bool, bool> NCALL;
  NCALL * ncallptr = new NCALL(& NdbScanOperation::close, args);
  ncallptr->runAsync();
  return Undefined();
}

