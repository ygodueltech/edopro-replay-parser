/*
 * Copyright (c) 2021, Dylam De La Torre <dyxel04@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "parser.hpp"

#include <google/protobuf/arena.h>

#include <ygopen/client/board.hpp>
#include <ygopen/client/card.hpp>
#include <ygopen/client/frame.hpp>
#include <ygopen/client/parse_event.hpp>
#include <ygopen/client/parse_query.hpp>
#include <ygopen/client/value_wrappers.hpp>
#include <ygopen/codec/encode_edo9300_ocgcore.hpp>
#include <ygopen/proto/replay.hpp>

namespace
{

using PBArena = google::protobuf::Arena;

class ReplayContext final : public YGOpen::Codec::IEncodeContext
{
public:
	ReplayContext()
		: board_()
		, arena_()
		, replay_(*PBArena::CreateMessage<YGOpen::Proto::Replay>(&arena_))
		, last_msg_()
		, match_win_reason_(0)
		, left_()
	{}

	auto pile_size(Con con, Loc loc) const noexcept -> size_t override
	{
		return board_.frame().pile(con, loc).size();
	}

	auto get_match_win_reason() const noexcept -> uint32_t override
	{
		return match_win_reason_;
	}

	auto has_xyz_mat(Place const& p) const noexcept -> bool override
	{
		return !board_.frame().zone(p).materials.empty();
	}

	auto get_xyz_left(Place const& left) const noexcept -> Place override
	{
		return left_.find(left)->second;
	}

	auto match_win_reason(uint32_t reason) noexcept -> void override
	{
		match_win_reason_ = reason;
	}

	auto xyz_mat_defer(Place const& place) noexcept -> void override
	{
		deferred_.emplace_back(place);
	}

	auto take_deferred_xyz_mat() noexcept -> std::vector<Place> override
	{
		decltype(deferred_) taken{};
		std::swap(taken, deferred_);
		return taken;
	}

	virtual auto xyz_left(Place const& left, Place const& from) noexcept
		-> void override
	{
		left_[left] = from;
	}

	auto arena() noexcept -> google::protobuf::Arena& { return arena_; }

	auto parse(YGOpen::Proto::Duel::Msg& msg) noexcept -> void
	{
		// Append message to the stream.
		{
			auto* block = replay_.mutable_stream()->add_blocks();
			block->set_time_offset_ms(0U);
			block->unsafe_arena_set_allocated_msg(&msg);
		}
		if(msg.t_case() == YGOpen::Proto::Duel::Msg::kEvent)
			parse_event(board_, msg.event());
		using namespace YGOpen::Client;
		auto& queries = *msg.mutable_queries();
		auto it = queries.begin();
		while(it != queries.end())
		{
			// Remove queries that do not point to a card.
			// Needed for old replays.
			if(!board_.frame().has_card(it->place()))
			{
				it = queries.erase(it);
				continue;
			}
			auto const hits = parse_query<true>(board_.frame(), *it);
			auto* data = it->mutable_data();
			using namespace YGOpen::Client;
#define X(v, q)                           \
	do                                    \
	{                                     \
		if(!!(hits & (QueryCacheHit::q))) \
			data->clear_##v();            \
	} while(0)
			X(owner, OWNER);
			X(is_public, IS_PUBLIC);
			X(is_hidden, IS_HIDDEN);
			X(position, POSITION);
			X(cover, COVER);
			X(status, STATUS);
			X(code, CODE);
			X(alias, ALIAS);
			X(type, TYPE);
			X(level, LEVEL);
			X(xyz_rank, XYZ_RANK);
			X(attribute, ATTRIBUTE);
			X(race, RACE);
			X(base_atk, BASE_ATK);
			X(atk, ATK);
			X(base_def, BASE_DEF);
			X(def, DEF);
			X(pend_l_scale, PEND_L_SCALE);
			X(pend_r_scale, PEND_R_SCALE);
			X(link_rate, LINK_RATE);
			X(link_arrow, LINK_ARROW);
			X(counters, COUNTERS);
			X(equipped, EQUIPPED);
			X(relations, RELATIONS);
#undef X
			++it;
		}
	}

	auto serialize() noexcept -> std::string
	{
		return replay_.SerializeAsString();
	}

private:
	template<typename T>
	using WrapperType = YGOpen::Client::Value<T>;
	using CardType = YGOpen::Client::BasicCard<WrapperType>;
	using FrameType = YGOpen::Client::BasicFrame<CardType>;
	using BoardType = YGOpen::Client::BasicBoard<FrameType, WrapperType>;
	BoardType board_;
	PBArena arena_;
	YGOpen::Proto::Replay& replay_;

	// Encoder context data.
	uint32_t match_win_reason_;
	std::map<Place, Place, YGOpen::Proto::Duel::PlaceLess> left_;
	std::vector<Place> deferred_;
};

} // namespace

auto analyze(uint8_t* buffer, size_t size) noexcept -> std::string
{
	decltype(buffer) const sentry = buffer + size;
	ReplayContext ctx;
	for(;;)
	{
		if(sentry == buffer) // Consumed entire buffer.
			break;
		if(sentry < buffer + sizeof(uint8_t) + sizeof(uint32_t))
		{
			std::cerr << "yrp: Unexpectedly short size for next message.\n";
			std::exit(6U);
		}
		// NOTE: Replays have size and msg_type swapped for some reason, we do
		// that swap here before trying to encode.
		auto const [msg_type, msg_size] = [&]() -> std::pair<uint8_t, uint32_t>
		{
			uint8_t msg{};
			uint32_t size{};
			std::memcpy(&msg, buffer, sizeof(msg));
			std::memcpy(&size, buffer + sizeof(msg), sizeof(size));
			buffer += sizeof(size);
			std::memcpy(buffer, &msg, sizeof(msg));
			return {msg, size};
		}();
		// We do not parse old replay format message.
		constexpr uint8_t OLD_REPLAY_FORMAT = 231U;
		if(msg_type == OLD_REPLAY_FORMAT)
			break;
		// Actual encoding.
		using namespace YGOpen::Codec;
		auto r = Edo9300::OCGCore::encode_one(ctx.arena(), buffer);
		switch(r.state)
		{
		case EncodeOneResult::State::STATE_OK:
		{
			ctx.parse(*r.msg);
			break;
		}
		case EncodeOneResult::State::STATE_SPECIAL:
		{
			r = Edo9300::OCGCore::encode_one_special(ctx.arena(), ctx, buffer);
			if(r.state == EncodeOneResult::State::STATE_OK)
			{
				ctx.parse(*r.msg);
			}
			else if(r.state != EncodeOneResult::State::STATE_SWALLOWED)
			{
				std::exit(7U);
			}
			break;
		}
		case EncodeOneResult::State::STATE_SWALLOWED:
		{
			// NOTE: Don't care about non-special swallowed messages.
			break;
		}
		default: // EncodeOneResult::State::STATE_UNKNOWN
			std::cerr << "yrp: Encountered unknown core message number: ";
			std::cerr << static_cast<int>(msg_type) << ".\n";
			std::exit(8U);
		}
		assert((msg_size + 1U) == r.bytes_read);
		buffer += r.bytes_read;
	}
	return ctx.serialize();
}
