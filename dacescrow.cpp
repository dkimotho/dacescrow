#include <eosiolib/eosio.hpp>
#include <eosiolib/transaction.hpp>

#include <string>

#include "dacescrow.hpp"

using namespace eosio;
using namespace std;

namespace eosdac {

    time_point current_time_point() {
        const static time_point ct{ microseconds{ static_cast<int64_t>( current_time() ) } };
        return ct;
    }

    dacescrow::~dacescrow() {}


    ACTION dacescrow::transfer(name from,
                               name to,
                               asset quantity,
                               string memo) {

        if (to != _self){
            return;
        }

        require_auth(from);

        eosio_assert(quantity.symbol.raw() == symbol{"EOS", 4}.raw(), "Only EOS tokens");

        asset zero_asset{0, symbol{"EOS", 4}};

        auto by_sender = escrows.get_index<"bysender"_n>();

        uint8_t found = 0;

        for (auto esc_itr = by_sender.lower_bound(from.value), end_itr = by_sender.upper_bound(from.value); esc_itr != end_itr; ++esc_itr) {
            if (esc_itr->amount == zero_asset){

                by_sender.modify(esc_itr, from, [&](escrow_info &e) {
                    e.amount = quantity;
                });

                found = 1;

                break;
            }
        }

        eosio_assert(found, "Could not find existing escrow to deposit to, transfer cancelled");
    }


    ACTION dacescrow::init(name sender, name receiver, name arb, time_point_sec expires, string memo) {
        require_auth(sender);

        asset zero_asset{0, symbol{"EOS", 4}};

        auto by_sender = escrows.get_index<"bysender"_n>();

        for (auto esc_itr = by_sender.lower_bound(sender.value), end_itr = by_sender.upper_bound(sender.value); esc_itr != end_itr; ++esc_itr) {
            eosio_assert(esc_itr->amount != zero_asset, "You already have an empty escrow.  Either fill it or delete it");
        }

        escrows.emplace(sender, [&](escrow_info &p) {
            p.key = escrows.available_primary_key();
            p.sender = sender;
            p.receiver = receiver;
            p.arb = arb;
            p.amount = zero_asset;
            p.expires = expires;
            p.memo = memo;
        });
    }

    ACTION dacescrow::approve(uint64_t key, name approver) {
        require_auth(approver);

        auto esc_itr = escrows.find(key);
        eosio_assert(esc_itr != escrows.end(), "Could not find escrow with that index");

        eosio_assert(esc_itr->amount.amount > 0, "This has not been initialized with a transfer");

        eosio_assert(esc_itr->sender == approver || esc_itr->receiver == approver || esc_itr->arb == approver, "You are not involved in this escrow");

        auto approvals = esc_itr->approvals;
        eosio_assert(std::find(approvals.begin(), approvals.end(), approver) == approvals.end(), "You have already approved this escrow");

        escrows.modify(esc_itr, approver, [&](escrow_info &e){
            e.approvals.push_back(approver);
        });
    }

    ACTION dacescrow::unapprove(uint64_t key, name disapprover) {
        require_auth(disapprover);

        auto esc_itr = escrows.find(key);
        eosio_assert(esc_itr != escrows.end(), "Could not find escrow with that index");

        eosio_assert(esc_itr->sender == disapprover || esc_itr->receiver == disapprover || esc_itr->arb == disapprover, "You are not involved in this escrow");

        escrows.modify(esc_itr, name{0}, [&](escrow_info &e){
            auto existing = std::find(e.approvals.begin(), e.approvals.end(), disapprover);
            eosio_assert(existing != e.approvals.end(), "You have NOT approved this escrow");
            e.approvals.erase(existing);
        });
    }

    ACTION dacescrow::claim(uint64_t key) {

        auto esc_itr = escrows.find(key);
        eosio_assert(esc_itr != escrows.end(), "Could not find escrow with that index");

        require_auth(esc_itr->receiver);

        eosio_assert(esc_itr->amount.amount > 0, "This has not been initialized with a transfer");

        auto approvals = esc_itr->approvals;

        eosio_assert(approvals.size() >= 2, "This escrow has not received the required approvals to claim");

        //inline transfer the required funds
        eosio::action(
                eosio::permission_level{_self , "active"_n }, "eosio.token"_n, "transfer"_n,
                make_tuple( _self, esc_itr->sender, esc_itr->amount, esc_itr->memo)
        ).send();


        escrows.erase(esc_itr);
    }

    /*
     * Empties an unfilled escrow request
     */
    ACTION dacescrow::cancel(uint64_t key) {

        auto esc_itr = escrows.find(key);
        eosio_assert(esc_itr != escrows.end(), "Could not find escrow with that index");

        require_auth(esc_itr->sender);

        asset zero_asset{0, symbol{"EOS", 4}};

        eosio_assert(zero_asset == esc_itr->amount, "Amount is not zero, this escrow is locked down");

        escrows.erase(esc_itr);
    }

    /*
     * Allows the sender to withdraw the funds if there are not enough approvals and the escrow has expired
     */
    ACTION dacescrow::refund(uint64_t key) {

        auto esc_itr = escrows.find(key);
        eosio_assert(esc_itr != escrows.end(), "Could not find escrow with that index");

        require_auth(esc_itr->sender);

        eosio_assert(esc_itr->amount.amount > 0, "This has not been initialized with a transfer");

        time_point_sec time_now = time_point_sec(current_time_point());

        eosio_assert(time_now >= esc_itr->expires, "Escrow has not expired");
        eosio_assert(esc_itr->approvals.size() >= 2, "Escrow has not received the required number of approvals");


        eosio::action(
                eosio::permission_level{_self , "active"_n }, "eosio.token"_n, "transfer"_n,
                make_tuple( _self, esc_itr->sender, esc_itr->amount, esc_itr->memo)
        ).send();


        escrows.erase(esc_itr);
    }

    ACTION dacescrow::clean() {
        require_auth(_self);

        auto itr = escrows.begin();
        while (itr != escrows.end()){
            itr = escrows.erase(itr);
        }

    }
}

#define EOSIO_ABI_EX(TYPE, MEMBERS) \
extern "C" { \
   void apply( uint64_t receiver, uint64_t code, uint64_t action ) { \
      if( action == "onerror"_n.value) { \
         /* onerror is only valid if it is for the "eosio" code account and authorized by "eosio"'s "active permission */ \
         eosio_assert(code == "eosio"_n.value, "onerror action's are only valid from the \"eosio\" system account"); \
      } \
      auto self = receiver; \
      if( (code == self  && action != "transfer"_n.value) || (code == "eosio.token"_n.value && action == "transfer"_n.value) ) { \
         switch( action ) { \
            EOSIO_DISPATCH_HELPER( TYPE, MEMBERS ) \
         } \
         /* does not allow destructor of thiscontract to run: eosio_exit(0); */ \
      } \
   } \
}


EOSIO_ABI_EX(eosdac::dacescrow,
             (transfer)
             (init)
             (approve)
             (unapprove)
             (claim)
             (refund)
             (cancel)
             (clean)
)
