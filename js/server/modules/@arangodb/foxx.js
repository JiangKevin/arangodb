'use strict';

////////////////////////////////////////////////////////////////////////////////
/// @brief The Foxx Framework
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2013 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Lucas Dohmen
/// @author Copyright 2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

exports.Controller = require('@arangodb/foxx/controller').Controller;
exports.Model = require('@arangodb/foxx/model').Model;
exports.Repository = require('@arangodb/foxx/repository').Repository;
exports.queues = require('@arangodb/foxx/queues');
exports.createQuery = require('@arangodb/foxx/query').createQuery;
exports.toJSONSchema = require('@arangodb/foxx/schema').toJSONSchema;

const manager = require('@arangodb/foxx/manager');
const deprecated = require('@arangodb/deprecated');

exports.getExports = function (path) {
  return manager.requireApp('/' + path.replace(/(^\/+|\/+$)/, ''));
};
exports.requireApp = function (path) {
  deprecated('3.0', 'Foxx.requireApp has been renamed to Foxx.getExports.');
  return exports.getExports(path);
};

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

/// Local Variables:
/// mode: outline-minor
/// outline-regexp: "/// @brief\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\|/\\*jslint"
/// End: