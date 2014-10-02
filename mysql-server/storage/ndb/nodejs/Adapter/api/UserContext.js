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

/*global assert, unified_debug */

"use strict";

var commonDBTableHandler = require(path.join(spi_dir,"common","DBTableHandler.js")),
    apiSession = require("./Session.js"),
    sessionFactory = require("./SessionFactory.js"),
    mynode     = require("./mynode.js"),
    query      = require("./Query.js"),
    spi        = require(path.join(spi_dir,"SPI")),
    udebug     = unified_debug.getLogger("UserContext.js"),
    stats_module = require(path.join(api_dir, "./stats.js")),
    stats      = stats_module.getWriter(["api", "UserContext"]),
    util       = require("util");

function Promise() {
  // implement Promises/A+ http://promises-aplus.github.io/promises-spec/
  // until then is called, this is an empty promise with no performance impact
}

function emptyFulfilledCallback(result) {
  return result;
}
function emptyRejectedCallback(err) {
  throw err;
}
/** Fulfill or reject the original promise via "The Promise Resolution Procedure".
 * original_promise is the Promise from this implementation on which "then" was called
 * new_promise is the Promise from this implementation returned by "then"
 * if the fulfilled or rejected callback provided by "then" returns a promise, wire the new_result (thenable)
 *  to fulfill the new_promise when new_result is fulfilled
 *  or reject the new_promise when new_result is rejected
 * otherwise, if the callback provided by "then" returns a value, fulfill the new_promise with that value
 * if the callback provided by "then" throws an Error, reject the new_promise with that Error
 */
var thenPromiseFulfilledOrRejected = function(original_promise, fulfilled_or_rejected_callback, new_promise, result) {
  var new_result;
  try {
    udebug.log_detail(original_promise.name, 'thenPromiseFulfilledOrRejected before');
    new_result = fulfilled_or_rejected_callback.call(undefined, result);
    udebug.log_detail(original_promise.name, 'thenPromiseFulfilledOrRejected after', new_result);
    var new_result_type = typeof new_result;
    if ((new_result_type === 'object' && new_result_type != null) | new_result_type === 'function') { 
      // 2.3.3 if result is an object or function
      // 2.3 The Promise Resolution Procedure
      // 2.3.1 If promise and x refer to the same object, reject promise with a TypeError as the reason.
      if (new_result === original_promise) {
        throw new Error('TypeError: Promise Resolution Procedure 2.3.1');
      }
      // 2.3.2 If x is a promise, adopt its state; but we don't care since it's also a thenable
      var then;
      try {
        then = new_result.then;
      } catch (thenE) {
        // 2.2.3.2 If retrieving the property x.then results in a thrown exception e, 
        // reject promise with e as the reason.
        new_promise.reject(thenE);
        return;
      }
      if (typeof then === 'function') {
        // 2.3.3.3 If then is a function, call it with x as this, first argument resolvePromise, 
        // and second argument rejectPromise
        // 2.3.3.3.3 If both resolvePromise and rejectPromise are called, 
        // or multiple calls to the same argument are made, the first call takes precedence, 
        // and any further calls are ignored.
        try {
          then.call(new_result,
            // 2.3.3.3.1 If/when resolvePromise is called with a value y, run [[Resolve]](promise, y).
            function(result) {
            udebug.log_detail(original_promise.name, 'thenPromiseFulfilledOrRejected deferred fulfill callback', new_result);
              if (!new_promise.resolved) {
                new_promise.fulfill(result);
              }
            },
            // 2.3.3.3.2 If/when rejectPromise is called with a reason r, reject promise with r.
            function(err) {
              udebug.log_detail(original_promise.name, 'thenPromiseFulfilledOrRejected deferred reject callback', new_result);
              if (!new_promise.resolved) {
                new_promise.reject(err);
              }
            }
          );
        } catch (callE) {
          // 2.3.3.3.4 If calling then throws an exception e,
          // 2.3.3.3.4.1 If resolvePromise or rejectPromise have been called, ignore it.
          if (!new_promise.resolved) {
            // 2.3.3.3.4.2 Otherwise, reject promise with e as the reason.
            new_promise.reject(callE);
          }
        }
      } else {
        // 2.3.3.4 If then is not a function, fulfill promise with x.
        new_promise.fulfill(new_result);
      }
    } else {
      // 2.3.4 If x is not an object or function, fulfill promise with x.
      new_promise.fulfill(new_result);
    }
  } catch (fulfillE) {
    // 2.2.7.2 If either onFulfilled or onRejected throws an exception e,
    // promise2 must be rejected with e as the reason.
    new_promise.reject(fulfillE);
  }
  
};

Promise.prototype.then = function(fulfilled_callback, rejected_callback, progress_callback) {
  var self = this;
  if (!self) udebug.log_detail('Promise.then called with no this');
  // create a new promise to return from the "then" method
  var new_promise = new Promise();
  if (typeof self.fulfilled_callbacks === 'undefined') {
    self.fulfilled_callbacks = [];
    self.rejected_callbacks = [];
    self.progress_callbacks = [];
  }
  if (self.resolved) {
    var resolved_result;
    udebug.log_detail(this.name, 'UserContext.Promise.then resolved; err:', self.err);
    if (self.err) {
      // this promise was already rejected
      udebug.log_detail(self.name, 'UserContext.Promise.then resolved calling (delayed) rejected_callback', rejected_callback);
      global.setImmediate(function() {
        udebug.log_detail(self.name, 'UserContext.Promise.then resolved calling rejected_callback', fulfilled_callback);
        thenPromiseFulfilledOrRejected(self, rejected_callback, new_promise, self.err);
      });
    } else {
      // this promise was already fulfilled, possibly with a null or undefined result
      udebug.log_detail(self.name, 'UserContext.Promise.then resolved calling (delayed) fulfilled_callback', fulfilled_callback);
      global.setImmediate(function() {
        udebug.log_detail(self.name, 'UserContext.Promise.then resolved calling fulfilled_callback', fulfilled_callback);
        thenPromiseFulfilledOrRejected(self, fulfilled_callback, new_promise, self.result);
      });
    }
    return new_promise;
  }
  // create a closure for each fulfilled_callback
  // the closure is a function that when called, calls setImmediate to call the fulfilled_callback with the result
  if (typeof fulfilled_callback === 'function') {
    udebug.log_detail(self.name, 'UserContext.Promise.then with fulfilled_callback', fulfilled_callback);
    // the following function closes (this, fulfilled_callback, new_promise)
    // and is called asynchronously when this promise is fulfilled
    this.fulfilled_callbacks.push(function(result) {
      global.setImmediate(function() {
        thenPromiseFulfilledOrRejected(self, fulfilled_callback, new_promise, result);
      });
    });
  } else {
    udebug.log_detail(self.name, 'UserContext.Promise.then with no fulfilled_callback');
    // create a dummy function for a missing fulfilled callback per 2.2.7.3 
    // If onFulfilled is not a function and promise1 is fulfilled, promise2 must be fulfilled with the same value.
    this.fulfilled_callbacks.push(function(result) {
      global.setImmediate(function() {
        thenPromiseFulfilledOrRejected(self, emptyFulfilledCallback, new_promise, result);
      });
    });
  }

  // create a closure for each rejected_callback
  // the closure is a function that when called, calls setImmediate to call the rejected_callback with the error
  if (typeof rejected_callback === 'function') {
    udebug.log_detail(self.name, 'UserContext.Promise.then with rejected_callback', rejected_callback);
    this.rejected_callbacks.push(function(err) {
      global.setImmediate(function() {
        thenPromiseFulfilledOrRejected(self, rejected_callback, new_promise, err);
      });
    });
  } else {
    udebug.log_detail(self.name, 'UserContext.Promise.then with no rejected_callback');
    // create a dummy function for a missing rejected callback per 2.2.7.4 
    // If onRejected is not a function and promise1 is rejected, promise2 must be rejected with the same reason.
    this.rejected_callbacks.push(function(err) {
      global.setImmediate(function() {
        thenPromiseFulfilledOrRejected(self, emptyRejectedCallback, new_promise, err);
      });
    });
  }
  // todo: progress_callbacks
  if (typeof progress_callback === 'function') {
    this.progress_callbacks.push(progress_callback);
  }

  return new_promise;
};

Promise.prototype.fulfill = function(result) {
  var name = this?this.name: 'no this';
  if (udebug.is_detail()) {
    udebug.log_detail(new Error(name, 'Promise.fulfill').stack);
  }
  if (this.resolved) {
    throw new Error('Fatal User Exception: fulfill called after fulfill or reject');
  }
  udebug.log_detail(name, 'Promise.fulfill with result', result, 'fulfilled_callbacks length:', 
      this.fulfilled_callbacks?  this.fulfilled_callbacks.length: 0);
  this.resolved = true;
  this.result = result;
  var fulfilled_callback;
  if (this.fulfilled_callbacks) {
    while(this.fulfilled_callbacks.length > 0) {
      fulfilled_callback = this.fulfilled_callbacks.shift();
      udebug.log_detail('Promise.fulfill for', result);
      fulfilled_callback(result);
    }
  }
};

Promise.prototype.reject = function(err) {
  if (this.resolved) {
    throw new Error('Fatal User Exception: reject called after fulfill or reject');
  }
  var name = this?this.name: 'no this';
  udebug.log_detail(name, 'Promise.reject with err', err, 'rejected_callbacks length:', 
      this.rejected_callbacks?  this.rejected_callbacks.length: 0);
  this.resolved = true;
  this.err = err;
  var rejected_callback;
  if (this.rejected_callbacks) {
    while(this.rejected_callbacks.length > 0) {
      rejected_callback = this.rejected_callbacks.shift();
      udebug.log_detail('Promise.reject for', err);
      rejected_callback(err);
    }
  }
//  throw err;
};

/** Create a function to manage the context of a user's asynchronous call.
 * All asynchronous user functions make a callback passing
 * the user's extra parameters from the original call as extra parameters
 * beyond the specified parameters of the call. For example, the persist function
 * is specified to take two parameters: the data object itself and the callback.
 * The result of persist is to call back with parameters of an error object, 
 * and the same data object which was passed. 
 * If extra parameters are passed to the persist function, the user's function
 * will be called with the specified parameters plus all extra parameters from
 * the original call. 
 * The constructor remembers the original user callback function and the original
 * parameters of the function.
 * The user callback function is always the last required parameter of the function call.
 * Additional context is added as the function progresses.
 * @param user_arguments the original arguments as supplied by the user
 * @param required_parameter_count the number of required parameters 
 * NOTE: the user callback function must be the last of the required parameters
 * @param returned_parameter_count the number of required parameters returned to the callback
 * @param session the Session which may be null for SessionFactory functions
 * @param session_factory the SessionFactory which may be null for Session functions
 * @param execute (optional; defaults to true) whether to execute the operation immediately;
 *        if execute is false, the operation is constructed and is available via the "operation"
 *        property of the user context.
 */
exports.UserContext = function(user_arguments, required_parameter_count, returned_parameter_count,
    session, session_factory, execute) {
  this.execute = (typeof execute === 'boolean' ? execute : true);
  this.user_arguments = user_arguments;
  this.user_callback = user_arguments[required_parameter_count - 1];
  this.required_parameter_count = required_parameter_count;
  this.extra_arguments_count = user_arguments.length - required_parameter_count;
  this.returned_parameter_count = returned_parameter_count;
  this.session = session;
  this.session_factory = session_factory;
  /* indicates that batch.clear was called before this context had executed */
  this.clear = false;
  if (this.session !== null) {
    this.autocommit = !this.session.tx.isActive();
  }
  this.errorMessages = '';
  this.promise = new Promise();
};

exports.UserContext.prototype.appendErrorMessage = function(message) {
  this.errorMessages += '\n' + message;
};

/** Get table metadata.
 * Delegate to DBConnectionPool.getTableMetadata.
 */
exports.UserContext.prototype.getTableMetadata = function() {
  var userContext = this;
  var getTableMetadataOnTableMetadata = function(err, tableMetadata) {
    userContext.applyCallback(err, tableMetadata);
  };
  
  var databaseName = this.user_arguments[0];
  var tableName = this.user_arguments[1];
  var dbSession = (this.session)?this.session.dbSession:null;
  this.session_factory.dbConnectionPool.getTableMetadata(
      databaseName, tableName, dbSession, getTableMetadataOnTableMetadata);
  return userContext.promise;
};


/** List all tables in the default database.
 * Delegate to DBConnectionPool.listTables.
 */
exports.UserContext.prototype.listTables = function() {
  var userContext = this;
  var listTablesOnTableList = function(err, tableList) {
    userContext.applyCallback(err, tableList);
  };

  var databaseName = this.user_arguments[0];
  var dbSession = (this.session)?this.session.dbSession:null;
  this.session_factory.dbConnectionPool.listTables(databaseName, dbSession, listTablesOnTableList);
  return userContext.promise;
};


/** Resolve properties. Properties might be an object, a name, or null.
 * If null, use all default properties. If a name, use default properties
 * of the named service provider. Otherwise, return the properties object.
 */
var resolveProperties = function(properties) {
  // Properties can be a string adapter name.  It defaults to 'ndb'.
  if(typeof properties === 'string') {
    properties = spi.getDBServiceProvider(properties).getDefaultConnectionProperties();
  }
  else if (properties === null) {
    properties = spi.getDBServiceProvider('ndb').getDefaultConnectionProperties();
  }
  return properties;
};

function getTableSpecification(defaultDatabaseName, tableName) {
  var split = tableName.split('\.');
  var result = {};
  if (split.length == 2) {
    result.dbName = split[0];
    result.unqualifiedTableName = split[1];
    result.qualifiedTableName = tableName;
  } else {
    // if split.length is not 1 then this error will be caught later
    result.dbName = defaultDatabaseName;
    result.unqualifiedTableName = tableName;
    result.qualifiedTableName = defaultDatabaseName + '.' + tableName;
  }
  return result;
}
/** Get the table handler for a domain object, table name, or constructor.
 */
var getTableHandler = function(domainObjectTableNameOrConstructor, session, onTableHandler) {

  // the table name might be qualified if the mapping specified a qualified table name
  // if unqualified, use sessionFactory.properties.database to qualify the table name
  var TableHandlerFactory = function(mynode, tableSpecification,
      sessionFactory, dbSession, mapping, ctor, onTableHandler) {
    this.sessionFactory = sessionFactory;
    this.dbSession = dbSession;
    this.onTableHandler = onTableHandler;
    this.mapping = mapping;
    this.mynode = mynode;
    this.ctor = ctor;
    this.tableSpecification = tableSpecification;
    stats.incr("TableHandlerFactory");
    
    this.createTableHandler = function() {
      var tableHandlerFactory = this;
      var tableHandler;
      var tableMetadata;
      
      var onTableMetadata = function(err, tableMetadata) {
        var tableHandler;
        var tableKey = tableHandlerFactory.tableSpecification.qualifiedTableName;
        udebug.log_detail('TableHandlerFactory.onTableMetadata for ',
            tableHandlerFactory.tableSpecification.qualifiedTableName + ' with err: ' + err);
        if (err) {
          tableHandlerFactory.onTableHandler(err, null);
        } else {
          // check to see if the metadata has already been processed
          if (typeof tableHandlerFactory.sessionFactory.tableMetadatas[tableKey] === 'undefined') {
            // put the table metadata into the table metadata map
            tableHandlerFactory.sessionFactory.tableMetadatas[tableKey] = tableMetadata;
          }
          // we have the table metadata; now create the table handler if needed
          if (tableHandlerFactory.mapping === null) {
            // put the default table handler into the session factory
            if (typeof(tableHandlerFactory.sessionFactory.tableHandlers[tableKey]) === 'undefined') {
              udebug.log_detail('UserContext caching the table handler in the sessionFactory for ', 
                  tableHandlerFactory.tableName);
              tableHandler = new commonDBTableHandler.DBTableHandler(tableMetadata, tableHandlerFactory.mapping,
                  tableHandlerFactory.ctor);
              tableHandlerFactory.sessionFactory.tableHandlers[tableKey] = tableHandler;
            } else {
              tableHandler = tableHandlerFactory.sessionFactory.tableHandlers[tableKey];
              udebug.log_detail('UserContext got tableHandler but someone else put it in the cache first for ', 
                  tableHandlerFactory.tableName);
            }
          }
          if (tableHandlerFactory.ctor) {
            if (typeof(tableHandlerFactory.ctor.prototype.mynode.tableHandler) === 'undefined') {
              // if a domain object mapping, cache the table handler in the prototype
              stats.incr( [ "TableHandler","success" ] );
              tableHandler = new commonDBTableHandler.DBTableHandler(tableMetadata, tableHandlerFactory.mapping,
                  tableHandlerFactory.ctor);
              if (tableHandler.isValid) {
                tableHandlerFactory.ctor.prototype.mynode.tableHandler = tableHandler;
                udebug.log_detail('UserContext caching the table handler in the prototype for constructor.');
              } else {
                tableHandlerFactory.err = tableHandler.err;
                udebug.log_detail('UserContext got invalid tableHandler', tableHandler.errorMessages);
              }
            } else {
              tableHandler = tableHandlerFactory.ctor.prototype.mynode.tableHandler;
              stats.incr( [ "TableHandler","idempotent" ] );
              udebug.log_detail('UserContext got tableHandler but someone else put it in the prototype first.');
            }
          }
          tableHandlerFactory.onTableHandler(tableHandlerFactory.err, tableHandler);
        }
      };
      
      // start of createTableHandler
      
      // get the table metadata from the cache of table metadatas in session factory
      tableMetadata = 
        tableHandlerFactory.sessionFactory.tableMetadatas[tableHandlerFactory.tableSpecification.qualifiedTableName];
      if (tableMetadata) {
        // we already have cached the table metadata
        onTableMetadata(null, tableMetadata);
      } else {
        // get the table metadata from the db connection pool
        // getTableMetadata(dbSession, databaseName, tableName, callback(error, DBTable));
        udebug.log('TableHandlerFactory.createTableHandler for ', 
            tableHandlerFactory.tableSpecification.dbName,
            tableHandlerFactory.tableSpecification.unqualifiedTableName);
        this.sessionFactory.dbConnectionPool.getTableMetadata(
            tableHandlerFactory.tableSpecification.dbName,
            tableHandlerFactory.tableSpecification.unqualifiedTableName, session.dbSession, onTableMetadata);
      }
    };
  };
    
  // start of getTableHandler 
  var err, mynode, tableHandler, tableHandlerFactory, tableIndicatorType, tableSpecification;

  tableIndicatorType = typeof(domainObjectTableNameOrConstructor);
  if (tableIndicatorType === 'string') {
    udebug.log_detail('UserContext.getTableHandler for table ', domainObjectTableNameOrConstructor);
    tableSpecification = getTableSpecification(session.sessionFactory.properties.database,
        domainObjectTableNameOrConstructor);

    // parameter is a table name; look up in table name to table handler hash
    tableHandler = session.sessionFactory.tableHandlers[tableSpecification.qualifiedTableName];
    if (typeof(tableHandler) === 'undefined') {
      udebug.log('UserContext.getTableHandler did not find cached tableHandler for table ',
          tableSpecification.qualifiedTableName);
      // create a new table handler for a table name with no mapping
      // create a closure to create the table handler
      tableHandlerFactory = new TableHandlerFactory(
          null, tableSpecification, session.sessionFactory, session.dbSession,
          null, null, onTableHandler);
      tableHandlerFactory.createTableHandler(null);
    } else {
      udebug.log_detail('UserContext.getTableHandler found cached tableHandler for table ',
          tableSpecification.qualifiedTableName);
      // send back the tableHandler
      onTableHandler(null, tableHandler);
    }
  } else if (tableIndicatorType === 'function') {
    udebug.log_detail('UserContext.getTableHandler for constructor.');
    mynode = domainObjectTableNameOrConstructor.prototype.mynode;
    // parameter is a constructor; it must have been annotated already
    if (typeof(mynode) === 'undefined') {
      err = new Error('User exception: constructor must have been annotated.');
      onTableHandler(err, null);
    } else {
      tableHandler = mynode.tableHandler;
      if (typeof(tableHandler) === 'undefined') {
        udebug.log('UserContext.getTableHandler did not find cached tableHandler for constructor.',
            domainObjectTableNameOrConstructor);
        // create the tableHandler
        // getTableMetadata(dbSession, databaseName, tableName, callback(error, DBTable));
        tableSpecification = getTableSpecification(session.sessionFactory.properties.database, mynode.mapping.table);
        tableHandlerFactory = new TableHandlerFactory(
            mynode, tableSpecification, session.sessionFactory, session.dbSession, 
            mynode.mapping, domainObjectTableNameOrConstructor, onTableHandler);
        tableHandlerFactory.createTableHandler();
      } else {
        stats.incr( [ "TableHandler","cache_hit" ] );
        udebug.log_detail('UserContext.getTableHandler found cached tableHandler for constructor.');
        // prototype has been annotated; return the table handler
        onTableHandler(null, tableHandler);
      }
    }
  } else if (tableIndicatorType === 'object') {
    udebug.log_detail('UserContext.getTableHandler for domain object.');
    // parameter is a domain object; it must have been mapped already
    mynode = domainObjectTableNameOrConstructor.constructor.prototype.mynode;
    if (typeof(mynode) === 'undefined') {
      err = new Error('User exception: constructor must have been annotated.');
      onTableHandler(err, null);
    } else {
      tableHandler = mynode.tableHandler;
      if (typeof(tableHandler) === 'undefined') {
        udebug.log_detail('UserContext.getTableHandler did not find cached tableHandler for object\n',
            util.inspect(domainObjectTableNameOrConstructor),
            'constructor\n', domainObjectTableNameOrConstructor.constructor);
        tableSpecification = getTableSpecification(session.sessionFactory.properties.database, mynode.mapping.table);
        // create the tableHandler
        // getTableMetadata(dbSession, databaseName, tableName, callback(error, DBTable));
        tableHandlerFactory = new TableHandlerFactory(
            mynode, tableSpecification, session.sessionFactory, session.dbSession, 
            mynode.mapping, domainObjectTableNameOrConstructor.constructor, onTableHandler);
        tableHandlerFactory.createTableHandler();
      } else {
        udebug.log_detail('UserContext.getTableHandler found cached tableHandler for constructor.');
        // prototype has been annotated; return the table handler
        onTableHandler(null, tableHandler);
      }
    }
  } else {
    err = new Error('User error: parameter must be a domain object, string, or constructor function.');
    onTableHandler(err, null);
  }
};

/** Try to find an existing session factory by looking up the connection string
 * and database name. Failing that, create a db connection pool and create a session factory.
 * Multiple session factories share the same db connection pool.
 * This function is used by both connect and openSession.
 */
var getSessionFactory = function(userContext, properties, tableMappings, callback) {
  var database;
  var connectionKey;
  var connection;
  var factory;
  var newSession;
  var sp;
  var i;
  var m;

  var resolveTableMappingsOnSession = function(err, session) {
    var mappings = [];
    var mappingBeingResolved = 0;

    var resolveTableMappingsOnTableHandler = function(err, tableHandler) {
      udebug.log_detail('UserContext.resolveTableMappinsgOnTableHandler', mappingBeingResolved + 1,
          'of', mappings.length, mappings[mappingBeingResolved]);
      if (err) {
        userContext.appendErrorMessage(err);
      }
      if (++mappingBeingResolved === mappings.length || mappingBeingResolved > 10) {
        // close the session the hard way (not using UserContext)
        session.dbSession.close(function(err) {
          if (err) {
            callback(err, null);
          } else {
            // now remove the session from the session factory's open connections
            session.sessionFactory.closeSession(session.index);
            // mark this session as unusable
            session.closed = true;
            // if any errors during table mapping, report them
            if (userContext.errorMessages) {
              err = new Error(userContext.errorMessages);
              callback(err, null);
            } else {
              // no errors
              callback(null, factory);
            }
          }
        });
      } else {
        // get the table handler for the next one, and so on until all are done
        getTableHandler(mappings[mappingBeingResolved], session, resolveTableMappingsOnTableHandler);
      }
    };

    // resolveTableMappingsOnSession begins here
    
    var tableMappingsType = typeof(tableMappings);
    var tableMapping;
    var tableMappingType;
    switch (tableMappingsType) {
    case 'string': 
      mappings.push(tableMappings); 
      break;
    case 'function': 
      mappings.push(tableMappings);
      break;
    case 'object': 
      if (tableMappings.length) {
        for (m = 0; m < tableMappings.length; ++m) {
          tableMapping = tableMappings[m];
          tableMappingType = typeof(tableMapping);
          if (tableMappingType === 'function' || tableMappingType === 'string') {
            mappings.push(tableMapping);
          } else {
            userContext.appendErrorMessage('unknown table mapping' + util.inspect(tableMapping));
          }
        }
      } else {
        userContext.appendErrorMessage('unknown table mappings' + util.inspect(tableMappings));
      }
      break;
    default:
      userContext.appendErrorMessage('unknown table mappings' + util.inspect(tableMappings));
      break;
    }
    if (mappings.length === 0) {
      udebug.log_detail('resolveTableMappingsOnSession no mappings!');
      callback(null, factory);
    }
    // get table handler for the first; the callback will then do the next one...
    udebug.log_detail('getSessionFactory resolving mappings:', mappings);
    getTableHandler(mappings[0], session, resolveTableMappingsOnTableHandler);
  };

  var resolveTableMappingsAndCallback = function() {
    if (!tableMappings) {
      callback(null, factory);
    } else {
      // get a session the hard way (not using UserContext) to resolve mappings
      var sessionSlot = factory.allocateSessionSlot();
      factory.dbConnectionPool.getDBSession(userContext.session_index, function(err, dbSession) {
        var newSession = new apiSession.Session(sessionSlot, factory, dbSession);
        factory.sessions[sessionSlot] = newSession;
        resolveTableMappingsOnSession(err, newSession);
      });
    }
  };

  var createFactory = function(dbConnectionPool) {
    var newFactory;
    udebug.log('connect createFactory creating factory for', connectionKey, 'database', database);
    newFactory = new sessionFactory.SessionFactory(connectionKey, dbConnectionPool,
        properties, tableMappings, mynode.deleteFactory);
    return newFactory;
  };
  
  var dbConnectionPoolCreated_callback = function(error, dbConnectionPool) {
    if (connection.isConnecting) {
      // the first requester for this connection
      connection.isConnecting = false;
      if (error) {
        callback(error, null);
      } else {
        udebug.log('dbConnectionPool created for', connectionKey, 'database', database);
        connection.dbConnectionPool = dbConnectionPool;
        factory = createFactory(dbConnectionPool);
        connection.factories[database] = factory;
        connection.count++;
        resolveTableMappingsAndCallback();
      }
      // notify all others that the connection is now ready (or an error was signaled)
      for (i = 0; i < connection.waitingForConnection.length; ++i) {
        udebug.log_detail('dbConnectionPoolCreated_callback notifying...');
        connection.waitingForConnection[i](error, dbConnectionPool);
      }
    } else {
      // another user request created the dbConnectionPool and session factory
      if (error) {
        callback(error, null);
      } else {
        factory = connection.factories[database];
        resolveTableMappingsAndCallback();
      }
    }
  };

  // getSessionFactory starts here
  database = properties.database;
  connectionKey = mynode.getConnectionKey(properties);
  connection = mynode.getConnection(connectionKey);

  if(typeof(connection) === 'undefined') {
    // there is no connection yet using this connection key    
    udebug.log('connect connection does not exist; creating factory for',
               connectionKey, 'database', database);
    connection = mynode.newConnection(connectionKey);
    sp = spi.getDBServiceProvider(properties.implementation);
    sp.connect(properties, dbConnectionPoolCreated_callback);
  } else {
    // there is a connection, but is it already connected?
    if (connection.isConnecting) {
      // wait until the first requester for this connection completes
      udebug.log('connect waiting for db connection by another for', connectionKey, 'database', database);
      connection.waitingForConnection.push(dbConnectionPoolCreated_callback);
    } else {
      // there is a connection, but is there a SessionFactory for this database?
      factory = connection.factories[database];
      if (typeof(factory) === 'undefined') {
        // create a SessionFactory for the existing dbConnectionPool
        udebug.log('connect creating factory with existing', connectionKey, 'database', database);
        factory = createFactory(connection.dbConnectionPool);
        connection.factories[database] = factory;
        connection.count++;
      }
//    resolve all table mappings before returning
      resolveTableMappingsAndCallback();
    }
  }
  
};

exports.UserContext.prototype.connect = function() {
  var userContext = this;
  // properties might be null, a name, or a properties object
  this.user_arguments[0] = resolveProperties(this.user_arguments[0]);

  var connectOnSessionFactory = function(err, factory) {
    userContext.applyCallback(err, factory);
  };

  getSessionFactory(this, this.user_arguments[0], this.user_arguments[1], connectOnSessionFactory);
  return userContext.promise;
};

function checkOperation(err, dbOperation) {
  var sqlstate, message, result, result_code;
  result = null;
  result_code = null;
  message = 'Unknown Error';
  sqlstate = '22000';
  if (err) {
    return err;
  } else if (dbOperation.result.success !== true) {
    if(dbOperation.result.error) {
      sqlstate = dbOperation.result.error.sqlstate;
      message = dbOperation.result.error.message || 'Operation error';
      result_code = dbOperation.result.error.code;
    }
    result = new Error(message);
    result.code = result_code;
    result.sqlstate = sqlstate;
  }
  return result;
}

/** Find the object by key.
 * 
 */
exports.UserContext.prototype.find = function() {
  var userContext = this;
  var tableHandler;
  if (typeof(this.user_arguments[0]) === 'function') {
    userContext.domainObject = true;
  }

  function findOnResult(err, dbOperation) {
    udebug.log('find.findOnResult');
    var result, values;
    var error = checkOperation(err, dbOperation);
    if (error && dbOperation.result.error.sqlstate !== '02000') {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(err, null);
    } else {
      udebug.log_detail('findOnResult returning ', dbOperation.result.value);
      userContext.applyCallback(null, dbOperation.result.value);      
    }
  }

  function findOnTableHandler(err, dbTableHandler) {
    var dbSession, keys, index, transactionHandler;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err, null);
    } else {
      userContext.dbTableHandler = dbTableHandler;
      keys = userContext.user_arguments[1];
      index = dbTableHandler.getIndexHandler(keys, true);
      if (index === null) {
        err = new Error('UserContext.find unable to get an index to use for ' + JSON.stringify(keys));
        userContext.applyCallback(err, null);
      } else {
        // create the find operation and execute it
        dbSession = userContext.session.dbSession;
        transactionHandler = dbSession.getTransactionHandler();
        userContext.operation = dbSession.buildReadOperation(index, keys,
            transactionHandler, findOnResult);
        if (userContext.execute) {
          transactionHandler.execute([userContext.operation], function() {
            udebug.log_detail('find transactionHandler.execute callback.');
          });
        } else if (typeof(userContext.operationDefinedCallback) === 'function') {
          userContext.operationDefinedCallback(1);
        }
      }
    }
  }

  // find starts here
  // session.find(prototypeOrTableName, key, callback)
  // validate first two parameters must be defined
  if (userContext.user_arguments[0] === undefined || userContext.user_arguments[1] === undefined) {
    userContext.applyCallback(new Error('User error: find must have at least two arguments.'), null);
  } else {
    // get DBTableHandler for prototype/tableName
    getTableHandler(userContext.user_arguments[0], userContext.session, findOnTableHandler);
  }
  return userContext.promise;
};


/** Create a query object.
 * 
 */
exports.UserContext.prototype.createQuery = function() {
  var userContext = this;
  var tableHandler;
  var queryDomainType;

  function createQueryOnTableHandler(err, dbTableHandler) {
    if (err) {
      userContext.applyCallback(err, null);
    } else {
      // create the query domain type and bind it to this session
      queryDomainType = new query.QueryDomainType(userContext.session, dbTableHandler, userContext.domainObject);
      udebug.log_detail('UserContext.createQuery queryDomainType:', queryDomainType);
      userContext.applyCallback(null, queryDomainType);
    }
  }

  // createQuery starts here
  // session.createQuery(constructorOrTableName, callback)
  // if the first parameter is a query object then copy the interesting bits and create a new object
  if (this.user_arguments[0].mynode_query_domain_type) {
    // TODO make sure this sessionFactory === other.sessionFactory
    queryDomainType = new query.QueryDomainType(userContext.session);
  }
  // if the first parameter is a table name the query results will be literals
  // if not (constructor or domain object) the query results will be domain objects
  userContext.domainObject = typeof(this.user_arguments[0]) !== 'string';
  // get DBTableHandler for constructor/tableName
  getTableHandler(userContext.user_arguments[0], userContext.session, createQueryOnTableHandler);

  return userContext.promise;
};

/** maximum skip and limit parameters are some large number */
var MAX_SKIP = Math.pow(2, 52);
var MAX_LIMIT = Math.pow(2, 52);

/** Execute a query. 
 * 
 */
exports.UserContext.prototype.executeQuery = function(queryDomainType) {
  var userContext = this;
  var dbSession, transactionHandler, queryType;
  userContext.queryDomainType = queryDomainType;

  // transform query result
  function executeQueryKeyOnResult(err, dbOperation) {
    udebug.log('executeQuery.executeQueryPKOnResult');
    var result, values, resultList;
    var error = checkOperation(err, dbOperation);
    if (error) {
      userContext.applyCallback(error, null);
    } else {
      if (userContext.queryDomainType.mynode_query_domain_type.domainObject) {
        values = dbOperation.result.value;
        result = userContext.queryDomainType.mynode_query_domain_type.dbTableHandler.newResultObject(values);
      } else {
        result = dbOperation.result.value;
      }
      if (result !== null) {
        // TODO: filter in memory if the adapter didn't filter all conditions
        resultList = [result];
      } else {
        resultList = [];
      }
      userContext.applyCallback(null, resultList);      
    }
  }

  // transform query result
  function executeQueryScanOnResult(err, dbOperation) {
    udebug.log_detail('executeQuery.executeQueryScanOnResult');
    var result, values, resultList;
    var error = checkOperation(err, dbOperation);
    if (error) {
      userContext.applyCallback(error, null);
    } else {
      udebug.log_detail('executeQuery.executeQueryScanOnResult', dbOperation.result.value);
      // TODO: filter in memory if the adapter didn't filter all conditions
      userContext.applyCallback(null, dbOperation.result.value);      
    }
  }

  // executeScanQuery is used by both index scan and table scan
  var executeScanQuery = function() {
    // validate order, skip, and limit parameters
    var params = userContext.user_arguments[0];
    var orderToUpperCase;
    var order = params.order, skip = params.skip, limit = params.limit;
    var error;
    if (typeof(limit) !== 'undefined') {
      if (limit < 0 || limit > MAX_LIMIT) {
        // limit is out of valid range
        error = new Error('Bad limit parameter \'' + limit + '\'; limit must be >= 0 and <= ' + MAX_LIMIT + '.');
      }
    }
    if (typeof(skip) !== 'undefined') {
      if (skip < 0 || skip > MAX_SKIP) {
        // skip is out of valid range
        error = new Error('Bad skip parameter \'' + skip + '\'; skip must be >= 0 and <= ' + MAX_SKIP + '.');
      } else {
        if (!order) {
          // skip is in range but order is not specified
          error = new Error('Bad skip parameter \'' + skip + '\'; if skip is specified, then order must be specified.');
        }
      }
    }
    if (typeof(order) !== 'undefined') {
      if (typeof(order) !== 'string') {
        error = new Error('Bad order parameter \'' + order + '\'; order must be ignoreCase asc or desc.');
      } else {
        orderToUpperCase = order.toUpperCase();
        if (!(orderToUpperCase === 'ASC' || orderToUpperCase === 'DESC')) {
          error = new Error('Bad order parameter \'' + order + '\'; order must be ignoreCase asc or desc.');
        }
      }
    }
    if (error) {
      userContext.applyCallback(error, null);
    } else {
      dbSession = userContext.session.dbSession;
      transactionHandler = dbSession.getTransactionHandler();
      userContext.operation = dbSession.buildScanOperation(
          queryDomainType, userContext.user_arguments[0], transactionHandler,
          executeQueryScanOnResult);
      // TODO: this currently does not support batching
      transactionHandler.execute([userContext.operation], function() {
        udebug.log_detail('executeQueryPK transactionHandler.execute callback.');
      });
    }
//  if (userContext.execute) {
//  transactionHandler.execute([userContext.operation], function() {
//    udebug.log_detail('find transactionHandler.execute callback.');
//  });
//} else if (typeof(userContext.operationDefinedCallback) === 'function') {
//  userContext.operationDefinedCallback(1);
//}    
  };    

  // executeKeyQuery is used by both primary key and unique key
  var executeKeyQuery = function() {
    // create the find operation and execute it
    dbSession = userContext.session.dbSession;
    transactionHandler = dbSession.getTransactionHandler();
    var dbIndexHandler = queryDomainType.mynode_query_domain_type.queryHandler.candidateIndex.dbIndexHandler;
    var keys = queryDomainType.mynode_query_domain_type.queryHandler.getKeys(userContext.user_arguments[0]);
    userContext.operation = dbSession.buildReadOperation(dbIndexHandler, keys, transactionHandler,
        executeQueryKeyOnResult);
    // TODO: this currently does not support batching
    transactionHandler.execute([userContext.operation], function() {
      udebug.log_detail('executeQueryPK transactionHandler.execute callback.');
    });
//    if (userContext.execute) {
//      transactionHandler.execute([userContext.operation], function() {
//        udebug.log_detail('find transactionHandler.execute callback.');
//      });
//    } else if (typeof(userContext.operationDefinedCallback) === 'function') {
//      userContext.operationDefinedCallback(1);
//    }    
  };
  
  // executeQuery starts here
  // query.execute(parameters, callback)
  udebug.log('QueryDomainType.execute', queryDomainType.mynode_query_domain_type.predicate, 
      'with parameters', userContext.user_arguments[0]);
  // execute the query and call back user
  queryType = queryDomainType.mynode_query_domain_type.queryType;
  switch(queryType) {
  case 0: // primary key
    executeKeyQuery();
    break;

  case 1: // unique key
    executeKeyQuery();
    break;

  case 2: // index scan
    executeScanQuery();
    break;

  case 3: // table scan
    executeScanQuery();
    break;

  default: 
    throw new Error('FatalInternalException: queryType: ' + queryType + ' not supported');
  }
  
  return userContext.promise;
};


/** Persist the object.
 * 
 */
exports.UserContext.prototype.persist = function() {
  var userContext = this;
  var object;

  function persistOnResult(err, dbOperation) {
    udebug.log('persist.persistOnResult');
    // return any error code
    var error = checkOperation(err, dbOperation);
    if (error) {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(error);
    } else {
      if (dbOperation.result.autoincrementValue) {
        // put returned autoincrement value into object
        userContext.dbTableHandler.setAutoincrement(userContext.values, dbOperation.result.autoincrementValue);
      }
      userContext.applyCallback(null);      
    }
  }

  function persistOnTableHandler(err, dbTableHandler) {
    userContext.dbTableHandler = dbTableHandler;
    udebug.log_detail('UserContext.persist.persistOnTableHandler ' + err);
    var transactionHandler;
    var dbSession = userContext.session.dbSession;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err);
    } else {
      transactionHandler = dbSession.getTransactionHandler();
      userContext.operation = dbSession.buildInsertOperation(dbTableHandler, userContext.values, transactionHandler,
          persistOnResult);
      if (userContext.execute) {
        transactionHandler.execute([userContext.operation], function() {
          udebug.log_detail('persist transactionHandler.execute callback.');
        });
      } else if (typeof(userContext.operationDefinedCallback) === 'function') {
        userContext.operationDefinedCallback(1);
      }
    }
  }

  // persist starts here
  if (userContext.required_parameter_count === 2) {
    // persist(object, callback)
    userContext.values = userContext.user_arguments[0];
  } else if (userContext.required_parameter_count === 3) {
    // persist(tableNameOrConstructor, values, callback)
    userContext.values = userContext.user_arguments[1];
  } else {
    throw new Error(
        'Fatal internal error; wrong required_parameter_count ' + userContext.required_parameter_count);
  }
  // get DBTableHandler for table indicator (domain object, constructor, or table name)
  getTableHandler(userContext.user_arguments[0], userContext.session, persistOnTableHandler);
  return userContext.promise;
};

/** Save the object. If the row already exists, overwrite non-pk columns.
 * 
 */
exports.UserContext.prototype.save = function() {
  var userContext = this;
  var tableHandler, object, indexHandler;

  function saveOnResult(err, dbOperation) {
    // return any error code
    var error = checkOperation(err, dbOperation);
    if (error) {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(error);
    } else {
      userContext.applyCallback(null);      
    }
  }

  function saveOnTableHandler(err, dbTableHandler) {
    var transactionHandler;
    var dbSession = userContext.session.dbSession;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err);
    } else {
      transactionHandler = dbSession.getTransactionHandler();
      indexHandler = dbTableHandler.getIndexHandler(userContext.values);
      if (!indexHandler.dbIndex.isPrimaryKey) {
        userContext.applyCallback(
            new Error('Illegal argument: parameter of save must include all primary key columns.'));
        return;
      }
      userContext.operation = dbSession.buildWriteOperation(indexHandler, userContext.values, transactionHandler,
          saveOnResult);
      if (userContext.execute) {
        transactionHandler.execute([userContext.operation], function() {
        });
      } else if (typeof(userContext.operationDefinedCallback) === 'function') {
        userContext.operationDefinedCallback(1);
      }
    }
  }

  // save starts here

  if (userContext.required_parameter_count === 2) {
    // save(object, callback)
    userContext.values = userContext.user_arguments[0];
  } else if (userContext.required_parameter_count === 3) {
    // save(tableNameOrConstructor, values, callback)
    userContext.values = userContext.user_arguments[1];
  } else {
    throw new Error(
        'Fatal internal error; wrong required_parameter_count ' + userContext.required_parameter_count);
  }
  // get DBTableHandler for table indicator (domain object, constructor, or table name)
  getTableHandler(userContext.user_arguments[0], userContext.session, saveOnTableHandler);
  return userContext.promise;
};

/** Update the object.
 * 
 */
exports.UserContext.prototype.update = function() {
  var userContext = this;
  var tableHandler, object, indexHandler;

  function updateOnResult(err, dbOperation) {
    // return any error code
    var error = checkOperation(err, dbOperation);
    if (error) {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(error);
    } else {
      userContext.applyCallback(null);      
    }
  }

  function updateOnTableHandler(err, dbTableHandler) {
    var transactionHandler;
    var dbSession = userContext.session.dbSession;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err);
    } else {
      transactionHandler = dbSession.getTransactionHandler();
      indexHandler = dbTableHandler.getIndexHandler(userContext.keys);
      // for variant update(object, callback) the object must include all primary keys
      if (userContext.required_parameter_count === 2 && !indexHandler.dbIndex.isPrimaryKey) {
        userContext.applyCallback(
            new Error('Illegal argument: parameter of update must include all primary key columns.'));
        return;
      }
      userContext.operation = dbSession.buildUpdateOperation(indexHandler, userContext.keys,
          userContext.values, transactionHandler, updateOnResult);
      if (userContext.execute) {
        transactionHandler.execute([userContext.operation], function() {
        });
      } else if (typeof(userContext.operationDefinedCallback) === 'function') {
        userContext.operationDefinedCallback(1);
      }
    }
  }

  // update starts here

  if (userContext.required_parameter_count === 2) {
    // update(object, callback)
    userContext.keys = userContext.user_arguments[0];
    userContext.values = userContext.user_arguments[0];
  } else if (userContext.required_parameter_count === 4) {
    // update(tableNameOrConstructor, keys, values, callback)
    userContext.keys = userContext.user_arguments[1];
    userContext.values = userContext.user_arguments[2];
  } else {
    throw new Error(
        'Fatal internal error; wrong required_parameter_count ' + userContext.required_parameter_count);
  }
  // get DBTableHandler for table indicator (domain object, constructor, or table name)
  getTableHandler(userContext.user_arguments[0], userContext.session, updateOnTableHandler);
  return userContext.promise;
};

/** Load the object.
 * 
 */
exports.UserContext.prototype.load = function() {
  var userContext = this;
  var tableHandler;

  function loadOnResult(err, dbOperation) {
    udebug.log('load.loadOnResult');
    var result, values;
    var error = checkOperation(err, dbOperation);
    if (error) {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(err);
      return;
    }
    values = dbOperation.result.value;
    // apply the values to the parameter domain object
    userContext.dbTableHandler.setFields(userContext.user_arguments[0], values);
    userContext.applyCallback(null);      
  }

  function loadOnTableHandler(err, dbTableHandler) {
    var dbSession, keys, index, transactionHandler;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err);
    } else {
      userContext.dbTableHandler = dbTableHandler;
      // the domain object must provide PRIMARY or unique key
      keys = userContext.user_arguments[0];
      // ask getIndexHandler for only unique key indexes
      index = dbTableHandler.getIndexHandler(keys, true);
      if (index === null) {
        err = new Error('Illegal argument: load unable to get a unique index to use for ' + JSON.stringify(keys));
        userContext.applyCallback(err);
      } else {
        // create the load operation and execute it
        dbSession = userContext.session.dbSession;
        transactionHandler = dbSession.getTransactionHandler();
        userContext.operation = dbSession.buildReadOperation(index, keys, transactionHandler,
            loadOnResult);
        if (userContext.execute) {
          transactionHandler.execute([userContext.operation], function() {
            udebug.log_detail('load transactionHandler.execute callback.');
          });
        } else if (typeof(userContext.operationDefinedCallback) === 'function') {
          userContext.operationDefinedCallback(1);
        }
      }
    }
  }

  // load starts here
  // session.load(instance, callback)
  // get DBTableHandler for instance constructor
  if (typeof(userContext.user_arguments[0].mynode) !== 'object') {
    userContext.applyCallback(new Error('Illegal argument: load requires a mapped domain object.'));
    return;
  }
  var ctor = userContext.user_arguments[0].mynode.constructor;
  getTableHandler(ctor, userContext.session, loadOnTableHandler);
  return userContext.promise;
};

/** Remove the object.
 * 
 */
exports.UserContext.prototype.remove = function() {
  var userContext = this;
  var tableHandler, object;

  function removeOnResult(err, dbOperation) {
    udebug.log('remove.removeOnResult');
    // return any error code plus the original user object
    var error = checkOperation(err, dbOperation);
    if (error) {
      if (userContext.session.tx.isActive()) {
        userContext.session.tx.setRollbackOnly();
      }
      userContext.applyCallback(error);
    } else {
      var result = dbOperation.result.value;
      userContext.applyCallback(null);
    }
  }

  function removeOnTableHandler(err, dbTableHandler) {
    var transactionHandler, object, dbIndexHandler;
    var dbSession = userContext.session.dbSession;
    if (userContext.clear) {
      // if batch has been cleared, user callback has already been called
      return;
    }
    if (err) {
      userContext.applyCallback(err);
    } else {
      dbIndexHandler = dbTableHandler.getIndexHandler(userContext.keys, true);
      if (dbIndexHandler === null) {
        err = new Error('UserContext.remove unable to get an index to use for ' + JSON.stringify(userContext.keys));
        userContext.applyCallback(err);
      } else {
        transactionHandler = dbSession.getTransactionHandler();
        userContext.operation = dbSession.buildDeleteOperation(
            dbIndexHandler, userContext.keys, transactionHandler, removeOnResult);
        if (userContext.execute) {
          transactionHandler.execute([userContext.operation], function() {
            udebug.log_detail('remove transactionHandler.execute callback.');
          });
        } else if (typeof(userContext.operationDefinedCallback) === 'function') {
          userContext.operationDefinedCallback(1);
        }
      }
    }
  }

  // remove starts here

  if (userContext.required_parameter_count === 2) {
    // remove(object, callback)
    userContext.keys = userContext.user_arguments[0];
  } else if (userContext.required_parameter_count === 3) {
    // remove(tableNameOrConstructor, values, callback)
    userContext.keys = userContext.user_arguments[1];
  } else {
    throw new Error(
        'Fatal internal error; wrong required_parameter_count ' + userContext.required_parameter_count);
  }
  // get DBTableHandler for table indicator (domain object, constructor, or table name)
  getTableHandler(userContext.user_arguments[0], userContext.session, removeOnTableHandler);
  return userContext.promise;
};

/** Get Mapping
 * 
 */
exports.UserContext.prototype.getMapping = function() {
  var userContext = this;
  function getMappingOnTableHandler(err, dbTableHandler) {
    if (err) {
      userContext.applyCallback(err, null);
      return;
    }
    var mapping = dbTableHandler.resolvedMapping;
    userContext.applyCallback(null, mapping);
  }
  // getMapping starts here
  getTableHandler(userContext.user_arguments[0], userContext.session, getMappingOnTableHandler);  
  return userContext.promise;
};

/** Execute a batch
 * 
 */
exports.UserContext.prototype.executeBatch = function(operationContexts) {
  var userContext = this;
  userContext.operationContexts = operationContexts;
  userContext.numberOfOperations = operationContexts.length;
  userContext.numberOfOperationsDefined = 0;

  // all operations have been executed and their user callbacks called
  // now call the Batch.execute callback
  var executeBatchOnExecute = function(err) {
    userContext.applyCallback(err);
  };

  // wait here until all operations have been defined
  // if operations are not yet defined, the onTableHandler callback
  // will call this function after the operation is defined
  var executeBatchOnOperationDefined = function(definedOperationCount) {
    userContext.numberOfOperationsDefined += definedOperationCount;
    udebug.log_detail('UserContext.executeBatch expecting', userContext.numberOfOperations, 
        'operations with', userContext.numberOfOperationsDefined, 'already defined.');
    if (userContext.numberOfOperationsDefined === userContext.numberOfOperations) {
      var operations = [];
      // collect all operations from the operation contexts
      userContext.operationContexts.forEach(function(operationContext) {
        operations.push(operationContext.operation);
      });
      // execute the batch
      var transactionHandler;
      var dbSession;
      dbSession = userContext.session.dbSession;
      transactionHandler = dbSession.getTransactionHandler();
      transactionHandler.execute(operations, executeBatchOnExecute);
    }
  };

  // executeBatch starts here
  // if no operations in the batch, just call the user callback
  if (operationContexts.length == 0) {
    executeBatchOnExecute(null);
  } else {
    // make sure all operations are defined
    operationContexts.forEach(function(operationContext) {
      // is the operation already defined?
      if (typeof(operationContext.operation) !== 'undefined') {
        userContext.numberOfOperationsDefined++;
      } else {
        // the operation has not been defined yet; set a callback for when the operation is defined
        operationContext.operationDefinedCallback = executeBatchOnOperationDefined;
      }
    });
    // now execute the operations
    executeBatchOnOperationDefined(0);
  }
  return userContext.promise;
};

/** Commit an active transaction. 
 * 
 */
exports.UserContext.prototype.commit = function() {
  var userContext = this;

  var commitOnCommit = function(err) {
    udebug.log('UserContext.commitOnCommit.');
    userContext.session.tx.setState(userContext.session.tx.idle);
    userContext.applyCallback(err);
  };

  // commit begins here
  if (userContext.session.tx.isActive()) {
    udebug.log('UserContext.commit tx is active.');
    userContext.session.dbSession.commit(commitOnCommit);
  } else {
    userContext.applyCallback(
        new Error('Fatal Internal Exception: UserContext.commit with no active transaction.'));
  }
  return userContext.promise;
};


/** Roll back an active transaction. 
 * 
 */
exports.UserContext.prototype.rollback = function() {
  var userContext = this;

  var rollbackOnRollback = function(err) {
    udebug.log('UserContext.rollbackOnRollback.');
    userContext.session.tx.setState(userContext.session.tx.idle);
    userContext.applyCallback(err);
  };

  // rollback begins here
  if (userContext.session.tx.isActive()) {
    udebug.log('UserContext.rollback tx is active.');
    var transactionHandler = userContext.session.dbSession.getTransactionHandler();
    transactionHandler.rollback(rollbackOnRollback);
  } else {
    userContext.applyCallback(
        new Error('Fatal Internal Exception: UserContext.rollback with no active transaction.'));
  }
  return userContext.promise;
};


/** Open a session. Allocate a slot in the session factory sessions array.
 * Call the DBConnectionPool to create a new DBSession.
 * Wrap the DBSession in a new Session and return it to the user.
 * This function is called by both mynode.openSession (without a session factory)
 * and SessionFactory.openSession (with a session factory).
 */
exports.UserContext.prototype.openSession = function() {
  var userContext = this;

  var openSessionOnSession = function(err, dbSession) {
    if (err) {
      userContext.applyCallback(err, null);
    } else {
      userContext.session = new apiSession.Session(userContext.session_index, userContext.session_factory, dbSession);
      userContext.session_factory.sessions[userContext.session_index] = userContext.session;
      userContext.applyCallback(err, userContext.session);
    }
  };

  var openSessionOnSessionFactory = function(err, factory) {
    if (err) {
      userContext.applyCallback(err, null);
    } else {
      userContext.session_factory = factory;
      // allocate a new session slot in sessions
      userContext.session_index = userContext.session_factory.allocateSessionSlot();
      // get a new DBSession from the DBConnectionPool
      userContext.session_factory.dbConnectionPool.getDBSession(userContext.session_index, 
          openSessionOnSession);
    }
  };
  
  // openSession starts here
  if (userContext.session_factory) {
    openSessionOnSessionFactory(null, userContext.session_factory);
  } else {
    udebug.log_detail('openSession for', util.inspect(userContext));
    // properties might be null, a name, or a properties object
    userContext.user_arguments[0] = resolveProperties(userContext.user_arguments[0]);
    getSessionFactory(userContext, userContext.user_arguments[0], userContext.user_arguments[1], 
        openSessionOnSessionFactory);
  }
  return userContext.promise;
};

/** Close a session. Close the dbSession which might put the underlying connection
 * back into the connection pool. Then, remove the session from the session factory's
 * open connections.
 * 
 */
exports.UserContext.prototype.closeSession = function() {
  var userContext = this;

  var closeSessionOnDBSessionClose = function(err) {
    // now remove the session from the session factory's open connections
    userContext.session_factory.closeSession(userContext.session.index);
    // mark this session as unusable
    userContext.session.closed = true;
    userContext.applyCallback(err);
  };
  // first, close the dbSession
  userContext.session.dbSession.close(closeSessionOnDBSessionClose);
  return userContext.promise;
};


/** Complete the user function by calling back the user with the results of the function.
 * Apply the user callback using the current arguments and the extra parameters from the original function.
 * Create the args for the callback by copying the current arguments to this function. Then, copy
 * the extra parameters from the original function. Finally, call the user callback.
 * If there is no user callback, and there is an error (first argument to applyCallback)
 * throw the error.
 */
exports.UserContext.prototype.applyCallback = function(err, result) {
  if (arguments.length !== this.returned_parameter_count) {
    throw new Error(
        'Fatal internal exception: wrong parameter count ' + arguments.length +' for UserContext applyCallback' + 
        '; expected ' + this.returned_parameter_count);
  }
  // notify (either fulfill or reject) the promise
  if (err) {
    udebug.log_detail('UserContext.applyCallback.reject', err);
    this.promise.reject(err);
  } else {
    udebug.log_detail('UserContext.applyCallback.fulfill', result);
    this.promise.fulfill(result);
  }
  if (typeof(this.user_callback) === 'undefined') {
    udebug.log_detail('UserContext.applyCallback with no user_callback.');
    return;
  }
  var args = [];
  var i, j;
  for (i = 0; i < arguments.length; ++i) {
    args.push(arguments[i]);
  }
  for (j = this.required_parameter_count; j < this.user_arguments.length; ++j) {
    args.push(this.user_arguments[j]);
  }
  this.user_callback.apply(null, args);
};

exports.Promise = Promise;