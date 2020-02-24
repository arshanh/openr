/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.KvStore
namespace py3 openr.thrift

include "Dual.thrift"

const string kDefaultArea = "0"

// a value as reported in get replies/publications
struct Value {
  // current version of this value
  1: i64 version;
  // the node that set the value
  // NOTE: the numbering is on purpose to maintain
  // backward compat
  3: string originatorId
  // the actual value: nonexistent for TTL update
  2: optional binary value;
  // TTL in milliseconds
  4: i64 ttl;
  // current version of this TTL, used to update TTL
  // TTL can be updated to prevent a key from expiring and being deleted
  // Newer TTL is represented by higher ttlVersion
  // Reset to 0 when "value version" increments
  5: i64 ttlVersion = 0;
  // Hash associated with `tuple<version, originatorId, value>`. Clients
  // should leave it empty and as will be computed by KvStore on `KEY_SET`
  // operation.
  6: optional i64 hash;
}

typedef map<string, Value>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::Value>") KeyVals


enum Command {
  // NOTE: key-10 has been used in past

  // operations on keys in the store
  KEY_SET   = 1,
  KEY_GET   = 2,
  KEY_DUMP  = 3,
  HASH_DUMP = 7,
  COUNTERS_GET = 9,   // Return object will be Monitor::CounterMap
  DUAL = 10, // DUAL message
  FLOOD_TOPO_SET = 11, // set or unset flooding-topo child
  FLOOD_TOPO_GET = 12; // get flood-topology information

  AREAS_CONFIG_GET = 13; // get AreasConfig from kvstore

  // operations on the store peers
  PEER_ADD  = 4,
  PEER_DEL  = 5,
  PEER_DUMP = 6,
}

//
// Cmd params
//

// parameters for the KEY_SET command
struct KeySetParams {
  // NOTE: the struct is denormalized on purpose,
  // it may happen so we repeat originatorId
  // too many times...

  // NOTE: we retain the original number in thrift struct
  2: KeyVals keyVals;

  // Solicit for an ack. This can be set to false to make requests one-way
  3: bool solicitResponse = 1;

  // Optional attributes. List of nodes through which this publication has
  // traversed. Client shouldn't worry about this attribute.
  5: optional list<string> nodeIds;

  // optional flood root-id, indicating which SPT this publication should be
  // flooded on; if none, flood to all peers
  6: optional string floodRootId;

  // optional attribute to indicate timestamp when request is sent. This is
  // system timestamp in milliseconds since epoch
  7: optional i64 timestamp_ms
}

// parameters for the KEY_GET command
struct KeyGetParams {
  1: list<string> keys
}

// parameters for the KEY_DUMP command
// if request includes keyValHashes information from peer, only respsond with
// keyVals on which hash differs
// if keyValHashes is not specified, respond with flooding element to signal of
// DB change
struct KeyDumpParams {
  1: string prefix
  3: set<string> originatorIds
  2: optional KeyVals keyValHashes
}

// Peer's publication and command socket URLs
// This is used in peer add requests and in
// the dump results
struct PeerSpec {
  1: string pubUrl
  2: string cmdUrl
  // support flood optimization or not
  3: bool supportFloodOptimization = 0
}

typedef map<string, PeerSpec>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::PeerSpec>")
  PeersMap

// parameters for peer addition
struct PeerAddParams {
  // map from nodeName to peer spec; we expect to
  // learn nodeName from HELLO packets, as it MUST
  // match the name supplied with Publication message
  1: PeersMap peers
}

// peers to delete
struct PeerDelParams {
  1: list<string> peerNames
}

// set/unset flood-topo child
struct FloodTopoSetParams {
  // spanning tree root-id
  1: string rootId
  // from node-id
  2: string srcId
  // set/unset a spanning tree child
  3: bool setChild
  // action apply to all-roots or not
  // if true, rootId will be ignored and action will be applied to all roots
  4: optional bool allRoots
}

typedef set<string>
  (cpp.type = "std::unordered_set<std::string>") PeerNames

// single spanning tree information
struct SptInfo {
  // passive state or not
  1: bool passive
  // metric cost towards root
  2: i64 cost
  // optional parent if any (aka nexthop)
  3: optional string parent
  // a set of spt children
  4: PeerNames children
}

// map<root-id: SPT-info>
typedef map<string, SptInfo>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::SptInfo>")
  SptInfoMap

// all spanning tree(s) information
struct SptInfos {
  // map<root-id: SptInfo>
  1: SptInfoMap infos
  // all DUAL related counters
  2: Dual.DualCounters counters
  // current flood-root-id if any
  3: optional string floodRootId
  // current flooding peers
  4: PeerNames floodPeers
}


// List of 'areas'
struct AreasConfig {
  // list of areas configured
  1: set<string>  (cpp.type = "std::unordered_set<std::string>") areas
}

//
// KvStoreRequest specification
//

// a request to the server (tagged union)
struct KvStoreRequest {
  1: Command cmd
  2: optional KeySetParams keySetParams
  3: optional KeyGetParams keyGetParams
  6: optional KeyDumpParams keyDumpParams
  4: optional PeerAddParams peerAddParams
  5: optional PeerDelParams peerDelParams
  9: optional Dual.DualMessages dualMessages
  10: optional FloodTopoSetParams floodTopoSetParams
  // area identifier to identify the KvStoreDb instance
  11: optional string area
}

//
// Responses
//
// this is also used to respond to GET requests
struct Publication {
  // NOTE: the numbering is on purpose, to maintain backward compatibility
  2: KeyVals keyVals;
  // expired keys
  3: list<string> expiredKeys;

  // Optional attributes. List of nodes through which this publication has
  // traversed. Client shouldn't worry about this attribute.
  4: optional list<string> nodeIds;

  // a list of keys that needs to be updated
  // this is only used for full-sync respone to tell full-sync initiator to
  // send back keyVals that need to be updated
  5: optional list<string> tobeUpdatedKeys;

  // optional flood root-id, indicating which SPT this publication should be
  // flooded on; if none, flood to all peers
  6: optional string floodRootId;

  // area to which this publication belogs
  7: optional string area;
}

// Dump of the current peers: sent in
// response to any PEER_ command, so the
// caller can see the result of the request
struct PeerCmdReply {
  1: PeersMap peers
}
