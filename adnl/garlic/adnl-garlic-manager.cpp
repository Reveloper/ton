/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "adnl-garlic-manager.hpp"
#include "td/utils/Random.h"
#include "adnl/adnl-address-list.hpp"
#include "auto/tl/ton_api.hpp"

namespace ton {

namespace adnl {

AdnlGarlicManager::AdnlGarlicManager(AdnlNodeIdShort local_id, td::uint8 adnl_cat,
                                     td::actor::ActorId<Adnl> adnl,
                                     td::actor::ActorId<keyring::Keyring> keyring,
                                     td::actor::ActorId<overlay::Overlays> overlays,
                                     std::shared_ptr<dht::DhtGlobalConfig> dht_config)
    : local_id_(local_id)
    , adnl_cat_(adnl_cat)
    , adnl_(std::move(adnl))
    , keyring_(std::move(keyring))
    , overlays_(std::move(overlays))
    , dht_config_(std::move(dht_config)) {
}

void AdnlGarlicManager::start_up() {
  // Creating DHT node
  if (use_secret_dht()) {
    PrivateKey pk(privkeys::Ed25519::random());
    PublicKey pub = pk.compute_public_key();
    td::actor::send_closure(keyring_, &keyring::Keyring::add_key, pk, true, [](td::Unit) {});
    AdnlNodeIdFull dht_id(pub);
    create_secret_id(dht_id, [](td::Result<td::Unit>) {});
    auto R = dht::Dht::create_client(dht_id.compute_short_id(), "", dht_config_, keyring_, adnl_);
    if (R.is_error()) {
      LOG(ERROR) << "Failed to create secret DHT node: " << R.move_as_error();
      return;
    }
    secret_dht_node_ = R.move_as_ok();
    for (const auto& id : secret_ids_) {
      td::actor::send_closure(adnl_, &Adnl::set_custom_dht_node, id.first, secret_dht_node_.get());
    }
  }

  td::Bits256 X = create_hash_tl_object<ton_api::adnl_garlic_publicOverlayId>();
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();
  td::actor::send_closure(overlays_, &overlay::Overlays::create_public_overlay_external, local_id_,
                          overlay_id_full_.clone(), std::make_unique<overlay::Overlays::EmptyCallback>(),
                          overlay::OverlayPrivacyRules{}, R"({ "type": "garlic" })");
  alarm();
}

void AdnlGarlicManager::tear_down() {
  td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

void AdnlGarlicManager::send_packet(AdnlNodeIdShort src, td::IPAddress dst_ip, td::BufferSlice data) {
  if (connection_.empty()) {
    LOG(DEBUG) << "Failed to send packet: connection is not ready";
    return;
  }
  td::actor::send_closure(connection_, &Connection::send_packet, src, dst_ip, std::move(data));
}

void AdnlGarlicManager::create_secret_id(AdnlNodeIdFull id, td::Promise<td::Unit> promise) {
  AdnlNodeIdShort id_short = id.compute_short_id();
  if (secret_ids_.count(id_short)) {
    promise.set_error(td::Status::Error("Duplicate secret id"));
    return;
  }
  secret_ids_[id_short].id_full = id;
  td::actor::send_closure(adnl_, &Adnl::add_id_ex, std::move(id), addr_list_, adnl_cat_, local_id_mode());
  if (use_secret_dht() && !secret_dht_node_.empty()) {
    td::actor::send_closure(adnl_, &Adnl::set_custom_dht_node, id_short, secret_dht_node_.get());
  }
  promise.set_result(td::Unit());
}

void AdnlGarlicManager::alarm() {
  td::actor::send_closure(
      overlays_, &overlay::Overlays::get_overlay_random_peers_full, local_id_, overlay_id_, 8,
      [SelfId = actor_id(this)](td::Result<std::vector<AdnlNodeIdFull>> R) {
        if (R.is_ok()) {
          td::actor::send_closure(SelfId, &AdnlGarlicManager::got_servers_from_overlay, R.move_as_ok());
        } else {
          LOG(WARNING) << "Failed to get peers: " << R.move_as_error();
          td::actor::send_closure(SelfId, &AdnlGarlicManager::got_servers_from_overlay, std::vector<AdnlNodeIdFull>());
        }
      });
}

void AdnlGarlicManager::got_servers_from_overlay(std::vector<AdnlNodeIdFull> servers) {
  for (AdnlNodeIdFull& id_full : servers) {
    AdnlNodeIdShort id = id_full.compute_short_id();
    if (servers_.count(id)) {
      continue;
    }
    LOG(DEBUG) << "Adding server " << id;
    servers_[id].id_full = std::move(id_full);
  }
  try_create_connection();
  alarm_timestamp() = td::Timestamp::in(td::Random::fast(1.0, 2.0));
}

void AdnlGarlicManager::try_create_connection() {
  if (!connection_.empty()) {
    return;
  }
  // TODO: move some parameters to config
  if (servers_.size() < 3) {
    LOG(DEBUG) << "Too few servers (" << servers_.size() << ")";
    return;
  }
  size_t chain_size = 2;
  std::vector<AdnlNodeIdFull> chain;
  for (const auto& p : servers_) {
    chain.push_back(p.second.id_full);
  }
  for (size_t i = 0; i < chain_size; ++i) {
    std::swap(chain[i], chain[i + td::Random::secure_uint32() % (chain.size() - i)]);
  }
  chain.resize(chain_size);

  class Callback : public Connection::Callback {
   public:
    Callback(td::actor::ActorId<AdnlGarlicManager> id) : id_(std::move(id)) {
    }
    void on_ready(AdnlAddressList addr_list) override {
      td::actor::send_closure(id_, &AdnlGarlicManager::update_addr_list, std::move(addr_list));
    }
    void on_fail(AdnlNodeIdShort causer = AdnlNodeIdShort::zero()) override {
      td::actor::send_closure(id_, &AdnlGarlicManager::on_connection_fail, causer);
    }

   private:
    td::actor::ActorId<AdnlGarlicManager> id_;
  };
  connection_ = td::actor::create_actor<Connection>("adnlgarlicconn", local_id_, std::move(chain),
                                                    std::make_unique<Callback>(actor_id(this)), adnl_cat_, adnl_,
                                                    keyring_);
}

void AdnlGarlicManager::update_addr_list(AdnlAddressList addr_list) {
  for (const auto& p : secret_ids_) {
    td::actor::send_closure(adnl_, &Adnl::add_id_ex, p.second.id_full, addr_list, adnl_cat_, local_id_mode());
  }
  addr_list_ = std::move(addr_list);
}

void AdnlGarlicManager::on_connection_fail(AdnlNodeIdShort causer) {
  connection_.reset();
  try_create_connection();
}

}  // namespace adnl

}  // namespace ton
