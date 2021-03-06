/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

extern "C" {
#include <ovsdb_wrapper.h>
};
#include <ovs_tor_agent/tor_agent_init.h>
#include <ovsdb_client.h>
#include <ovsdb_client_idl.h>
#include <ovsdb_client_session.h>
#include <logical_switch_ovsdb.h>
#include <unicast_mac_remote_ovsdb.h>
#include <physical_locator_ovsdb.h>

#include <oper/vn.h>
#include <oper/vrf.h>
#include <oper/nexthop.h>
#include <oper/tunnel_nh.h>
#include <oper/agent_path.h>
#include <oper/bridge_route.h>
#include <ovsdb_types.h>

using OVSDB::UnicastMacRemoteEntry;
using OVSDB::UnicastMacRemoteTable;
using OVSDB::VrfOvsdbObject;
using OVSDB::OvsdbDBEntry;
using OVSDB::OvsdbDBObject;
using OVSDB::OvsdbClientSession;

UnicastMacRemoteEntry::UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
        const std::string mac) : OvsdbDBEntry(table), mac_(mac),
    logical_switch_name_(table->logical_switch_name()),
    self_exported_route_(false) {
}

UnicastMacRemoteEntry::UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
        const BridgeRouteEntry *entry) : OvsdbDBEntry(table),
        mac_(entry->mac().ToString()),
        logical_switch_name_(table->logical_switch_name()) {
}

UnicastMacRemoteEntry::UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
        const UnicastMacRemoteEntry *entry) : OvsdbDBEntry(table),
        mac_(entry->mac_), logical_switch_name_(entry->logical_switch_name_),
        dest_ip_(entry->dest_ip_) {
}

UnicastMacRemoteEntry::UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
        struct ovsdb_idl_row *entry) : OvsdbDBEntry(table, entry),
    mac_(ovsdb_wrapper_ucast_mac_remote_mac(entry)),
    logical_switch_name_(table->logical_switch_name()), dest_ip_() {
    const char *dest_ip = ovsdb_wrapper_ucast_mac_remote_dst_ip(entry);
    if (dest_ip) {
        dest_ip_ = std::string(dest_ip);
    }
};

void UnicastMacRemoteEntry::NotifyAdd(struct ovsdb_idl_row *row) {
    if (ovs_entry() == NULL || ovs_entry() == row) {
        // this is the first idl row to use let the base infra
        // use it.
        OvsdbDBEntry::NotifyAdd(row);
        return;
    }
    dup_list_.insert(row);
    // trigger change to delete duplicate entries
    table_->Change(this);
}

void UnicastMacRemoteEntry::NotifyDelete(struct ovsdb_idl_row *row) {
    if (ovs_entry() == row) {
        OvsdbDBEntry::NotifyDelete(row);
        return;
    }
    dup_list_.erase(row);
}

void UnicastMacRemoteEntry::PreAddChange() {
    boost::system::error_code ec;
    Ip4Address dest_ip = Ip4Address::from_string(dest_ip_, ec);
    LogicalSwitchTable *l_table = table_->client_idl()->logical_switch_table();
    LogicalSwitchEntry key(l_table, logical_switch_name_.c_str());
    LogicalSwitchEntry *logical_switch =
        static_cast<LogicalSwitchEntry *>(l_table->GetReference(&key));

    if (self_exported_route_ ||
            dest_ip == logical_switch->physical_switch_tunnel_ip()) {
        // if the route is self exported or if dest tunnel end-point points to
        // the physical switch itself then donot export this route to OVSDB
        // release reference to logical switch to trigger delete.
        logical_switch_ = NULL;
        return;
    }
    logical_switch_ = logical_switch;
}

void UnicastMacRemoteEntry::PostDelete() {
    logical_switch_ = NULL;
}

void UnicastMacRemoteEntry::AddMsg(struct ovsdb_idl_txn *txn) {
    if (!dup_list_.empty()) {
        // if we have entries in duplicate list clean up all
        // by encoding a delete message and on ack re-trigger
        // Add Msg to return to sane state.
        DeleteMsg(txn);
        return;
    }

    if (logical_switch_.get() == NULL) {
        DeleteMsg(txn);
        return;
    }
    if (ovs_entry_ == NULL && !dest_ip_.empty()) {
        PhysicalLocatorTable *pl_table =
            table_->client_idl()->physical_locator_table();
        PhysicalLocatorEntry pl_key(pl_table, dest_ip_);
        /*
         * we don't take reference to physical locator, just use if locator
         * is existing or we will create a new one.
         */
        PhysicalLocatorEntry *pl_entry =
            static_cast<PhysicalLocatorEntry *>(pl_table->Find(&pl_key));
        struct ovsdb_idl_row *pl_row = NULL;
        if (pl_entry)
            pl_row = pl_entry->ovs_entry();
        LogicalSwitchEntry *logical_switch =
            static_cast<LogicalSwitchEntry *>(logical_switch_.get());
        obvsdb_wrapper_add_ucast_mac_remote(txn, mac_.c_str(),
                logical_switch->ovs_entry(), pl_row, dest_ip_.c_str());
        SendTrace(UnicastMacRemoteEntry::ADD_REQ);
    }
}

void UnicastMacRemoteEntry::ChangeMsg(struct ovsdb_idl_txn *txn) {
    AddMsg(txn);
}

void UnicastMacRemoteEntry::DeleteMsg(struct ovsdb_idl_txn *txn) {
    DeleteDupEntries(txn);
    if (ovs_entry_) {
        ovsdb_wrapper_delete_ucast_mac_remote(ovs_entry_);
        SendTrace(UnicastMacRemoteEntry::DEL_REQ);
    }
}

void UnicastMacRemoteEntry::OvsdbChange() {
    if (!IsResolved())
        table_->NotifyEvent(this, KSyncEntry::ADD_CHANGE_REQ);
}

bool UnicastMacRemoteEntry::Sync(DBEntry *db_entry) {
    const BridgeRouteEntry *entry =
        static_cast<const BridgeRouteEntry *>(db_entry);
    std::string dest_ip;
    const NextHop *nh = entry->GetActiveNextHop();
    /* 
     * TOR Agent will not have any local VM so only tunnel nexthops
     * are to be looked into
     */
    if (nh && nh->GetType() == NextHop::TUNNEL) {
        /*
         * we don't care the about the tunnel type in nh and always program
         * the entry to ovsdb expecting vrouter to always handle
         * VxLAN encapsulation.
         */
        const TunnelNH *tunnel = static_cast<const TunnelNH *>(nh);
        dest_ip = tunnel->GetDip()->to_string();
    }
    bool change = false;
    if (dest_ip_ != dest_ip) {
        dest_ip_ = dest_ip;
        change = true;
    }

    // Since OVSDB exports routes to evpn table check for self exported route
    // path in the corresponding evpn route entry, instead of bridge entry
    VrfEntry *vrf = entry->vrf();
    EvpnAgentRouteTable *evpn_table =
        static_cast<EvpnAgentRouteTable *>(vrf->GetEvpnRouteTable());
    Ip4Address default_ip;
    EvpnRouteEntry *evpn_rt = evpn_table->FindRoute(entry->mac(), default_ip,
                                                    entry->GetActiveLabel());

    bool self_exported_route =
        (evpn_rt != NULL &&
         evpn_rt->FindPath((Peer *)table_->client_idl()->route_peer()) != NULL);
    if (self_exported_route_ != self_exported_route) {
        self_exported_route_ = self_exported_route;
        change = true;
    }
    return change;
}

bool UnicastMacRemoteEntry::IsLess(const KSyncEntry &entry) const {
    const UnicastMacRemoteEntry &ucast =
        static_cast<const UnicastMacRemoteEntry&>(entry);
    if (mac_ != ucast.mac_)
        return mac_ < ucast.mac_;
    return logical_switch_name_ < ucast.logical_switch_name_;
}

KSyncEntry *UnicastMacRemoteEntry::UnresolvedReference() {
    LogicalSwitchTable *l_table = table_->client_idl()->logical_switch_table();
    LogicalSwitchEntry key(l_table, logical_switch_name_.c_str());
    LogicalSwitchEntry *l_switch =
        static_cast<LogicalSwitchEntry *>(l_table->GetReference(&key));
    if (!l_switch->IsResolved()) {
        return l_switch;
    }
    return NULL;
}

const std::string &UnicastMacRemoteEntry::mac() const {
    return mac_;
}

const std::string &UnicastMacRemoteEntry::logical_switch_name() const {
    return logical_switch_name_;
}

const std::string &UnicastMacRemoteEntry::dest_ip() const {
    return dest_ip_;
}

bool UnicastMacRemoteEntry::self_exported_route() const {
    return self_exported_route_;
}

void UnicastMacRemoteEntry::SendTrace(Trace event) const {
    SandeshUnicastMacRemoteInfo info;
    switch (event) {
    case ADD_REQ:
        info.set_op("Add Requested");
        break;
    case DEL_REQ:
        info.set_op("Delete Requested");
        break;
    case ADD_ACK:
        info.set_op("Add Received");
        break;
    case DEL_ACK:
        info.set_op("Delete Received");
        break;
    default:
        info.set_op("unknown");
    }
    info.set_mac(mac_);
    info.set_logical_switch(logical_switch_name_);
    info.set_dest_ip(dest_ip_);
    OVSDB_TRACE(UnicastMacRemote, info);
}

// Always called from any message encode Add/Change/Delete
// to trigger delete for deleted entries.
void UnicastMacRemoteEntry::DeleteDupEntries(struct ovsdb_idl_txn *txn) {
    OvsdbDupIdlList::iterator it = dup_list_.begin();
    for (; it != dup_list_.end(); it++) {
        ovsdb_wrapper_delete_ucast_mac_remote(*it);
    }
}

UnicastMacRemoteTable::UnicastMacRemoteTable(OvsdbClientIdl *idl,
        AgentRouteTable *table, const std::string &logical_switch_name) :
    OvsdbDBObject(idl, table), logical_switch_name_(logical_switch_name),
    deleted_(false), table_delete_ref_(this, table->deleter()) {
}

UnicastMacRemoteTable::~UnicastMacRemoteTable() {
    // explicit unregister required before removing the reference, to assure
    // pointer sanity.
    UnregisterDb(GetDBTable());
    table_delete_ref_.Reset(NULL);
}

void UnicastMacRemoteTable::OvsdbNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    const char *mac = ovsdb_wrapper_ucast_mac_remote_mac(row);
    const char *logical_switch =
        ovsdb_wrapper_ucast_mac_remote_logical_switch(row);
    /* if logical switch is not available ignore nodtification */
    if (logical_switch == NULL)
        return;
    const char *dest_ip = ovsdb_wrapper_ucast_mac_remote_dst_ip(row);
    UnicastMacRemoteEntry key(this, mac);
    if (op == OvsdbClientIdl::OVSDB_DEL) {
        NotifyDeleteOvsdb((OvsdbDBEntry*)&key, row);
        if (dest_ip)
            key.dest_ip_ = std::string(dest_ip);
        key.SendTrace(UnicastMacRemoteEntry::DEL_ACK);
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        NotifyAddOvsdb((OvsdbDBEntry*)&key, row);
        if (dest_ip)
            key.dest_ip_ = std::string(dest_ip);
        key.SendTrace(UnicastMacRemoteEntry::ADD_ACK);
    } else {
        assert(0);
    }
}

KSyncEntry *UnicastMacRemoteTable::Alloc(const KSyncEntry *key, uint32_t index) {
    const UnicastMacRemoteEntry *k_entry =
        static_cast<const UnicastMacRemoteEntry *>(key);
    UnicastMacRemoteEntry *entry = new UnicastMacRemoteEntry(this, k_entry);
    return entry;
}

KSyncEntry *UnicastMacRemoteTable::DBToKSyncEntry(const DBEntry* db_entry) {
    const BridgeRouteEntry *entry =
        static_cast<const BridgeRouteEntry *>(db_entry);
    UnicastMacRemoteEntry *key = new UnicastMacRemoteEntry(this, entry);
    return static_cast<KSyncEntry *>(key);
}

OvsdbDBEntry *UnicastMacRemoteTable::AllocOvsEntry(struct ovsdb_idl_row *row) {
    UnicastMacRemoteEntry key(this, row);
    return static_cast<OvsdbDBEntry *>(Create(&key));
}

KSyncDBObject::DBFilterResp UnicastMacRemoteTable::DBEntryFilter(
        const DBEntry *db_entry) {
    // Since Object delete for unicast remote table happens by db
    // walk on vrf table, it needs to implement filter to ignore
    // db Add/Change notifications if idl is marked deleted.
    if (client_idl()->deleted()) {
        return DBFilterIgnore;
    }

    const BridgeRouteEntry *entry =
        static_cast<const BridgeRouteEntry *>(db_entry);
    if (entry->vrf()->IsDeleted()) {
        // if notification comes for a entry with deleted vrf,
        // trigger delete since we donot resue same vrf object
        // so this entry has to be deleted eventually.
        return DBFilterDelete;
    }

    return DBFilterAccept;
}

void UnicastMacRemoteTable::ManagedDelete() {
    deleted_ = true;
    Unregister();
}

void UnicastMacRemoteTable::Unregister() {
    if (IsEmpty() == true && deleted_ == true) {
        KSyncObjectManager::Unregister(this);
    }
}

void UnicastMacRemoteTable::EmptyTable() {
    OvsdbDBObject::EmptyTable();
    // unregister the object if emptytable is called with
    // object being scheduled for delete, or if managed
    // delete is triggered on the object.
    if (delete_scheduled() || deleted_ == true) {
        Unregister();
    }
}

const std::string &UnicastMacRemoteTable::logical_switch_name() const {
    return logical_switch_name_;
}

void UnicastMacRemoteTable::set_deleted(bool deleted) {
    deleted_ = deleted;
}

bool UnicastMacRemoteTable::deleted() {
    return deleted_;
}

VrfOvsdbObject::VrfOvsdbObject(OvsdbClientIdl *idl, DBTable *table) :
    client_idl_(idl), table_(table), deleted_(false),
    walkid_(DBTableWalker::kInvalidWalkerId) {
    vrf_listener_id_ = table->Register(boost::bind(&VrfOvsdbObject::VrfNotify,
                this, _1, _2));

    // Trigger Walk to get existing vrf entries.
    DBTableWalker *walker = idl->agent()->db()->GetWalker();
    walkid_ = walker->WalkTable(table_, NULL,
            boost::bind(&VrfOvsdbObject::VrfWalkNotify, this, _1, _2),
            boost::bind(&VrfOvsdbObject::VrfWalkDone, this, _1));

    client_idl_->Register(OvsdbClientIdl::OVSDB_UCAST_MAC_REMOTE,
            boost::bind(&VrfOvsdbObject::OvsdbRouteNotify, this, _1, _2));
}

VrfOvsdbObject::~VrfOvsdbObject() {
    assert(walkid_ == DBTableWalker::kInvalidWalkerId);
    table_->Unregister(vrf_listener_id_);
}

void VrfOvsdbObject::OvsdbRouteNotify(OvsdbClientIdl::Op op,
        struct ovsdb_idl_row *row) {
    const char *mac = ovsdb_wrapper_ucast_mac_remote_mac(row);
    const char *logical_switch =
        ovsdb_wrapper_ucast_mac_remote_logical_switch(row);
    /* if logical switch is not available ignore notification */
    if (logical_switch == NULL)
        return;
    LogicalSwitchMap::iterator it = logical_switch_map_.find(logical_switch);
    if (it == logical_switch_map_.end()) {
        // if we fail to find ksync object, encode and send delete.
        struct ovsdb_idl_txn *txn = client_idl_->CreateTxn(NULL);
        if (txn == NULL) {
            // failed to create transaction because of idl marked for
            // deletion return from here.
            return;
        }
        ovsdb_wrapper_delete_ucast_mac_remote(row);
        struct jsonrpc_msg *msg = ovsdb_wrapper_idl_txn_encode(txn);
        if (msg == NULL) {
            client_idl_->DeleteTxn(txn);
        } else {
            client_idl_->SendJsonRpc(msg);
        }
        return;
    }
    const char *dest_ip = ovsdb_wrapper_ucast_mac_remote_dst_ip(row);
    UnicastMacRemoteTable *table= it->second->l2_table;
    UnicastMacRemoteEntry key(table, mac);
    if (op == OvsdbClientIdl::OVSDB_DEL) {
        table->NotifyDeleteOvsdb((OvsdbDBEntry*)&key, row);
        if (dest_ip)
            key.dest_ip_ = std::string(dest_ip);
        key.SendTrace(UnicastMacRemoteEntry::DEL_ACK);
    } else if (op == OvsdbClientIdl::OVSDB_ADD) {
        table->NotifyAddOvsdb((OvsdbDBEntry*)&key, row);
        if (dest_ip)
            key.dest_ip_ = std::string(dest_ip);
        key.SendTrace(UnicastMacRemoteEntry::ADD_ACK);
    } else {
        assert(0);
    }
}

// Start a walk on Vrf table and trigger ksync object delete for all the
// route tables.
void VrfOvsdbObject::DeleteTable() {
    if (deleted_)
        return;
    deleted_ = true;
    DBTableWalker *walker = client_idl_->agent()->db()->GetWalker();
    if (walkid_ != DBTableWalker::kInvalidWalkerId) {
        walker->WalkCancel(walkid_);
    }
    walkid_ = walker->WalkTable(table_, NULL,
            boost::bind(&VrfOvsdbObject::VrfWalkNotify, this, _1, _2),
            boost::bind(&VrfOvsdbObject::VrfWalkDone, this, _1));
}

bool VrfOvsdbObject::VrfWalkNotify(DBTablePartBase *partition,
                                   DBEntryBase *entry) {
    VrfNotify(partition, entry);
    return true;
}

void VrfOvsdbObject::VrfWalkDone(DBTableBase *partition) {
    walkid_ = DBTableWalker::kInvalidWalkerId;
    if (deleted_) {
        client_idl_->UnRegister(OvsdbClientIdl::OVSDB_UCAST_MAC_REMOTE);
        client_idl_ = NULL;
    }
}

void VrfOvsdbObject::VrfNotify(DBTablePartBase *partition, DBEntryBase *e) {
    VrfEntry *vrf = static_cast<VrfEntry *>(e);
    VrfState *state = static_cast<VrfState *>
        (vrf->GetState(partition->parent(), vrf_listener_id_));

    // Trigger delete of route table in following cases:
    //  - VrfOvsdbObject is scheduled for deletion.
    //  - VrfEntry is deleted
    //  - VRF-VN link not available
    if (deleted_ || vrf->IsDeleted() || (vrf->vn() == NULL)) {
        if (state) {
            // Vrf Object is marked for delete trigger delete for l2 table
            // object and cleanup vrf state.
            state->l2_table->DeleteTable();

            // Clear DB state
            logical_switch_map_.erase(state->logical_switch_name_);
            vrf->ClearState(partition->parent(), vrf_listener_id_);
            delete state;
        }
        return;
    }

    if (state == NULL) {
        state = new VrfState();
        state->logical_switch_name_ = UuidToString(vrf->vn()->GetUuid());
        // Assumption one vn maps only to one vrf
        logical_switch_map_[state->logical_switch_name_] = state;
        vrf->SetState(partition->parent(), vrf_listener_id_, state);

        /* We are interested only in L2 Routes */
        state->l2_table = new UnicastMacRemoteTable(client_idl_.get(),
                vrf->GetBridgeRouteTable(), state->logical_switch_name_);
    } else {
        // verify that logical switch name doesnot change
        assert(state->logical_switch_name_ ==
               UuidToString(vrf->vn()->GetUuid()));
    }
}

const VrfOvsdbObject::LogicalSwitchMap &
VrfOvsdbObject::logical_switch_map() const {
    return logical_switch_map_;
}

/////////////////////////////////////////////////////////////////////////////
// Sandesh routines
/////////////////////////////////////////////////////////////////////////////
class UnicastMacRemoteSandeshTask : public Task {
public:
    UnicastMacRemoteSandeshTask(std::string resp_ctx, const std::string &ip,
                                uint32_t port) :
        Task((TaskScheduler::GetInstance()->GetTaskId("Agent::KSync")), 0),
        resp_(new OvsdbUnicastMacRemoteResp()), resp_data_(resp_ctx),
        ip_(ip), port_(port) {
    }
    virtual ~UnicastMacRemoteSandeshTask() {}
    virtual bool Run() {
        std::vector<OvsdbUnicastMacRemoteEntry> macs;
        TorAgentInit *init =
            static_cast<TorAgentInit *>(Agent::GetInstance()->agent_init());
        OvsdbClientSession *session;
        if (ip_.empty()) {
            session = init->ovsdb_client()->NextSession(NULL);
        } else {
            boost::system::error_code ec;
            Ip4Address ip_addr = Ip4Address::from_string(ip_, ec);
            session = init->ovsdb_client()->FindSession(ip_addr, port_);
        }
        if (session != NULL && session->client_idl() != NULL) {
            VrfOvsdbObject *vrf_obj = session->client_idl()->vrf_ovsdb();
            const VrfOvsdbObject::LogicalSwitchMap ls_table =
                vrf_obj->logical_switch_map();
            VrfOvsdbObject::LogicalSwitchMap::const_iterator it =
                ls_table.begin();
            for (; it != ls_table.end(); it++) {
                UnicastMacRemoteTable *table = it->second->l2_table;
                UnicastMacRemoteEntry *entry =
                    static_cast<UnicastMacRemoteEntry *>(table->Next(NULL));
                while (entry != NULL) {
                    OvsdbUnicastMacRemoteEntry oentry;
                    oentry.set_state(entry->StateString());
                    oentry.set_mac(entry->mac());
                    oentry.set_logical_switch(entry->logical_switch_name());
                    oentry.set_dest_ip(entry->dest_ip());
                    oentry.set_self_exported(entry->self_exported_route());
                    macs.push_back(oentry);
                    entry =
                        static_cast<UnicastMacRemoteEntry *>(table->Next(entry));
                }
            }
        }
        resp_->set_macs(macs);
        SendResponse();
        return true;
    }
private:
    void SendResponse() {
        resp_->set_context(resp_data_);
        resp_->set_more(false);
        resp_->Response();
    }

    OvsdbUnicastMacRemoteResp *resp_;
    std::string resp_data_;
    std::string ip_;
    uint32_t port_;
    DISALLOW_COPY_AND_ASSIGN(UnicastMacRemoteSandeshTask);
};

void OvsdbUnicastMacRemoteReq::HandleRequest() const {
    UnicastMacRemoteSandeshTask *task =
        new UnicastMacRemoteSandeshTask(context(), get_session_remote_ip(),
                                        get_session_remote_port());
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Enqueue(task);
}

