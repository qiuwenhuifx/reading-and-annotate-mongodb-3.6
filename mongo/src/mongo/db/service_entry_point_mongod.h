/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/transport/service_entry_point_impl.h"

namespace mongo {

/**
 * The entry point into mongod. Just a wrapper around assembleResponse.
 */
 
//ServiceContextMongoD->ServiceContext(包含ServiceEntryPoint成员)
//ServiceEntryPointMongod->ServiceEntryPointImpl->ServiceEntryPoint

//_initAndListen->（serviceContext->setServiceEntryPoint）中构造使用该类, 存入ServiceContextMongoD::ServiceContext._serviceEntryPoint
//class ServiceEntryPointMongod final : public ServiceEntryPointImpl { //原始定义
//mongod服务入口点
class ServiceEntryPointMongod : public ServiceEntryPointImpl {//yang change
    MONGO_DISALLOW_COPYING(ServiceEntryPointMongod);

public:
    using ServiceEntryPointImpl::ServiceEntryPointImpl;
    
    //ServiceEntryPointMongod::handleRequest(mongod网络处理)  ServiceEntryPointMongos::handleRequest mongos网络请求处理
    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override;
};

}  // namespace mongo
