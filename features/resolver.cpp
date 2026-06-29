#include <algorithm>
#include <random>
#include "../include_cheat.h"

namespace
{
	enum class resolver_state_t
	{
		STATE_STANDING = 0,
		STATE_WALKING,
		STATE_RUNNING,
		STATE_CROUCHING,
		STATE_CROUCH_MOVING,
		STATE_IN_AIR
	};

	const char* state_names[] = {
		"STANDING",
		"WALKING",
		"RUNNING",
		"CROUCHING",
		"CROUCH_MOVING",
		"IN_AIR"
	};

	// Anti-aim classification used by the adaptive bruteforce. Cached on
	// player_log_t::m_resolver_state each resolve so apply_desync_side and
	// get_brute_direction see the same verdict within a tick.
	enum class enemy_aa_state_t
	{
		state_normal = 0,
		state_jitter,
		state_static,
		state_low_delta,
		state_fake_flick
	};

	const char* aa_state_names[] = {
		"NORMAL",
		"JITTER",
		"STATIC",
		"LOW_DELTA",
		"FAKE_FLICK"
	};

	bool is_sideways( C_CSPlayer* player, const lag_record_t* record )
	{
		if ( !local_player )
			return false;

		const auto at_target_yaw = math::calc_angle( local_player->get_origin(), player->get_origin() ).y;
		const auto delta = fabsf( math::normalize_float( record->m_eye_angles.y - at_target_yaw ) );

		return ( delta > 45.f && delta < 135.f ) || ( delta > 225.f && delta < 315.f );
	}

	float get_avg_yaw( const player_log_t& log )
	{
		if ( log.record.empty() )
			return 0.f;

		const auto limit = std::min( static_cast< size_t >( log.record.size() ), static_cast< size_t >( 16 ) );
		auto sum = 0.f;

		for ( auto i = log.record.size() - limit; i < log.record.size(); i++ )
			sum += log.record[ i ].m_eye_angles.y;

		return sum / static_cast< float >( limit );
	}

	float get_forward_yaw( C_CSPlayer* player )
	{
		if ( !local_player )
			return 0.f;

		return math::calc_angle( local_player->get_origin(), player->get_origin() ).y;
	}

	float get_recent_yaw_span( const player_log_t& log, const size_t sample_count = 6 )
	{
		if ( log.record.size() < 2 )
			return 0.f;

		const auto limit = std::min( static_cast< size_t >( log.record.size() ), sample_count );
		const auto base_yaw = log.record[ log.record.size() - 1 ].m_eye_angles.y;

		auto span = 0.f;
		for ( auto i = log.record.size() - limit; i < log.record.size(); i++ )
			span = std::max( span, fabsf( math::angle_diff( log.record[ i ].m_eye_angles.y, base_yaw ) ) );

		return span * 2.f;
	}

	bool is_jittering( const player_log_t& log )
	{
		if ( log.record.size() < 3 )
			return false;

		const auto limit = std::min( static_cast< size_t >( log.record.size() ), static_cast< size_t >( 6 ) );
		auto large_swings = 0;
		auto avg_delta = 0.f;
		auto max_delta = 0.f;

		for ( auto i = log.record.size() - limit + 1; i < log.record.size(); i++ )
		{
			const auto delta = fabsf( math::angle_diff( log.record[ i ].m_eye_angles.y, log.record[ i - 1 ].m_eye_angles.y ) );
			if ( !std::isnan( delta ) )
			{
				avg_delta += delta;
				max_delta = std::max( max_delta, delta );

				if ( delta > 35.f )
					large_swings++;
			}
		}

		avg_delta /= static_cast< float >( limit - 1 );
		return large_swings >= 2 && max_delta > 70.f && avg_delta > 28.f;
	}

	bool is_static_antiaim( const player_log_t& log )
	{
		return get_recent_yaw_span( log, 5 ) < 20.f;
	}

	bool is_defensive_tick( const lag_record_t* current, const lag_record_t* previous )
	{
		if ( !current )
			return false;

		if ( current->m_breaking_lc || current->m_ignore || current->m_tickbase_shift || current->m_extrapolated || current->m_lagamt > 10 )
			return true;

		return previous && current->m_simtime <= previous->m_simtime;
	}

	bool is_fake_flick( const lag_record_t* current, const lag_record_t* previous )
	{
		if ( !current || !previous )
			return false;

		const auto speed = current->m_velocity.Length2D();
		const auto yaw_delta = fabsf( math::angle_diff( current->m_eye_angles.y, previous->m_eye_angles.y ) );
		const auto lby_delta = fabsf( math::angle_diff( current->m_eye_angles.y, current->m_lby ) );

		return speed < 36.f && yaw_delta > 90.f && lby_delta > 35.f;
	}

	// LBY break detection. When the enemy flicks their lower body yaw to a
	// new value (delta > 5°), the foot yaw snaps to the new LBY and the
	// desync resets to the LBY side for the next ~0.22s. This is a strong
	// signal the resolver should follow instead of trusting the previous
	// desync side or the freestand/layer heuristics (which are noisy during
	// the break transition). Only valid on ground — in air, LBY doesn't
	// update, so a delta is just interpolation/network noise.
	bool is_breaking_lby( const lag_record_t* current, const lag_record_t* previous )
	{
		if ( !current || !previous )
			return false;

		if ( !( current->m_flags & FL_ONGROUND ) )
			return false;

		const auto lby_delta = fabsf( math::angle_diff( current->m_lby, previous->m_lby ) );
		return lby_delta > 5.f;
	}

	bool is_low_delta_record( const lag_record_t* record, const float foot_yaw )
	{
		if ( !record )
			return false;

		const auto eye_to_foot = fabsf( math::angle_diff( record->m_eye_angles.y, foot_yaw ) );
		const auto eye_to_lby = fabsf( math::angle_diff( record->m_eye_angles.y, record->m_lby ) );
		return eye_to_foot < 30.f || eye_to_lby < 35.f;
	}

	bool is_extended_desync( const lag_record_t* record )
	{
		if ( !record )
			return false;

		const auto& state = record->m_state[ resolver_direction::resolver_networked ].m_animstate;
		const auto modifier = std::clamp( record->m_yaw_modifier > 0.f ? record->m_yaw_modifier : 1.f, 0.35f, 1.25f );
		const auto max_range = std::max( fabsf( state.aim_yaw_max ), fabsf( state.aim_yaw_min ) ) * modifier * 2.f;
		const auto eye_delta = fabsf( math::angle_diff( record->m_eye_angles.y, state.foot_yaw ) );

		return eye_delta > 43.f || max_range > 45.f;
	}

	// Classify the enemy's anti-aim from the recent record history. Returns a
	// single verdict combining the existing jitter / static / low-delta / fake
	// flick detectors. Order matters: fake-flick is highest priority (it
	// implies a sub-tick yaw jump that overrides the others), then jitter,
	// then low-delta, then static; anything else is "normal".
	enemy_aa_state_t classify_enemy_state( player_log_t& log, const lag_record_t* current, const lag_record_t* previous )
	{
		if ( !current )
			return enemy_aa_state_t::state_normal;

		if ( is_fake_flick( current, previous ) )
			return enemy_aa_state_t::state_fake_flick;

		if ( is_jittering( log ) )
			return enemy_aa_state_t::state_jitter;

		const auto& animstate = current->m_state[ resolver_direction::resolver_networked ].m_animstate;
		if ( is_low_delta_record( current, animstate.foot_yaw ) )
			return enemy_aa_state_t::state_low_delta;

		if ( is_static_antiaim( log ) )
			return enemy_aa_state_t::state_static;

		return enemy_aa_state_t::state_normal;
	}

	resolver_direction side_to_direction( const int side )
	{
		if ( side > 0 )
			return resolver_direction::resolver_max;

		if ( side < 0 )
			return resolver_direction::resolver_min;

		return resolver_direction::resolver_zero;
	}

	resolver_direction opposite_direction( const resolver_direction direction )
	{
		switch ( direction )
		{
			case resolver_direction::resolver_max:
				return resolver_direction::resolver_min;
			case resolver_direction::resolver_min:
				return resolver_direction::resolver_max;
			default:
				return resolver_direction::resolver_zero;
		}
	}

	float get_direction_delta( const lag_record_t& record, const resolver_direction direction )
	{
		const auto& animstate = record.m_state[ resolver_direction::resolver_networked ].m_animstate;
		const auto modifier = std::clamp( record.m_yaw_modifier > 0.f ? record.m_yaw_modifier : 1.f, 0.35f, 1.25f );

		switch ( direction )
		{
			case resolver_direction::resolver_max:
				return std::clamp( animstate.aim_yaw_max * modifier * 2.f, 0.f, 58.f );
			case resolver_direction::resolver_min:
				return std::clamp( animstate.aim_yaw_min * modifier * 2.f, -58.f, 0.f );
			default:
				return 0.f;
		}
	}

	float build_server_abs_yaw( const lag_record_t& record, const float eye_yaw, const float foot_yaw )
	{
		const auto& animstate = record.m_state[ resolver_direction::resolver_networked ].m_animstate;
		const auto speed = std::min( record.m_velocity.Length2D(), 260.f );
		const auto running_speed = std::clamp( speed / ( 260.f * .52f ), 0.f, 1.f );
		const auto ducking_speed = std::clamp( speed / ( 260.f * .34f ), 0.f, 1.f );
		const auto duck_amount = std::clamp( record.m_duckamt + animstate.duck_additional, 0.f, 1.f );
		const auto update_time = ticks_to_time( std::max( record.m_lagamt, 1 ) );

		auto yaw_modifier = ( ( ( animstate.ground_fraction * -.3f ) - .2f ) * running_speed ) + 1.f;
		if ( duck_amount > 0.f )
			yaw_modifier += ( duck_amount * ducking_speed ) * ( .5f - yaw_modifier );

		const auto min_yaw = animstate.aim_yaw_min * yaw_modifier * 2.f;
		const auto max_yaw = animstate.aim_yaw_max * yaw_modifier * 2.f;
		const auto eye_feet_delta = math::angle_diff( eye_yaw, foot_yaw );

		auto goal_feet_yaw = foot_yaw;
		if ( eye_feet_delta > max_yaw )
			goal_feet_yaw = eye_yaw - fabsf( max_yaw );
		else if ( eye_feet_delta < min_yaw )
			goal_feet_yaw = eye_yaw + fabsf( min_yaw );

		if ( speed > .1f || fabsf( record.m_velocity.z ) > 100.f )
			goal_feet_yaw = math::approach_angle( eye_yaw, goal_feet_yaw, ( ( animstate.running_speed * 20.f ) + 30.f ) * update_time );
		else
			goal_feet_yaw = math::approach_angle( record.m_lby, goal_feet_yaw, update_time * 100.f );

		return math::normalize_float( goal_feet_yaw );
	}

	resolver_direction direction_from_yaw( const lag_record_t& record, const float target_yaw, const float eye_angle )
	{
		auto closest = resolver_direction::resolver_networked;
		auto closest_delta = FLT_MAX;

		for ( auto i = static_cast< int >( resolver_direction::resolver_networked ); i < static_cast< int >( resolver_direction::resolver_direction_max ); i++ )
		{
			const auto direction = static_cast< resolver_direction >( i );
			const auto simulated_yaw = resolver::get_resolver_angle( record, direction, eye_angle );
			const auto diff = fabsf( math::angle_diff( simulated_yaw, target_yaw ) );

			if ( diff < closest_delta )
			{
				closest_delta = diff;
				closest = direction;
			}
		}

		return closest;
	}

	void refresh_low_delta_state( player_log_t& log, const lag_record_t* record, const float foot_yaw )
	{
		if ( !is_low_delta_record( record, foot_yaw ) )
			return;

		// Dynamic low-delta offsets. Modern low-delta desync is typically
		// 10-25 degrees, so a hardcoded 35 degree offset misses the head
		// hitbox entirely. Scale the brute offsets from the enemy's actual
		// maximum desync (derived from the animstate yaw limits and the
		// record's yaw modifier) instead.
		const auto& animstate = record->m_state[ resolver_direction::resolver_networked ].m_animstate;
		const auto modifier = std::clamp( record->m_yaw_modifier > 0.f ? record->m_yaw_modifier : 1.f, 0.35f, 1.25f );
		const auto max_desync = std::clamp( std::max( fabsf( animstate.aim_yaw_max ), fabsf( animstate.aim_yaw_min ) ) * modifier * 2.f, 0.f, 58.f );

		const auto left_low = math::normalize_float( record->m_lby - max_desync * 0.3f );
		const auto right_low = math::normalize_float( record->m_lby + max_desync * 0.6f );

		switch ( log.m_shots % 3 )
		{
			case 1:
				log.m_stored_brute_yaw = left_low;
				break;
			case 2:
				log.m_stored_brute_yaw = right_low;
				break;
			default:
				break;
		}
	}

	int detect_freestand( C_CSPlayer* player, lag_record_t* record, const player_log_t& log )
	{
		if ( !local_player || log.record.size() < 2 )
			return 0;

		const auto eye_pos = record->m_origin + Vector( 0.f, 0.f, 64.f - record->m_duckamt * 16.f );

		auto forward = local_player->get_origin() - player->get_origin();
		forward.z = 0.f;
		if ( forward.Length2DSqr() < 1.f )
			return 0;

		forward.Normalize();

		auto yaw = record->m_eye_angles.y;
		if ( record->m_velocity.Length2D() > 1.f )
			yaw = get_avg_yaw( log );

		Vector right{};
		math::angle_vectors( QAngle( 0.f, yaw + 90.f, 0.f ), nullptr, &right, nullptr );

		const auto neg_pos = eye_pos - right * 23.f;
		const auto pos_pos = eye_pos + right * 23.f;

		CTraceFilterWorldOnly filter{};
		trace_t neg_trace{};
		trace_t pos_trace{};
		trace_t center_trace{};
		Ray_t ray{};

		ray.Init( neg_pos, local_player->get_eye_pos() );
		interfaces::engine_trace()->TraceRay( ray, MASK_SHOT_HULL | CONTENTS_GRATE, &filter, &neg_trace );

		ray.Init( pos_pos, local_player->get_eye_pos() );
		interfaces::engine_trace()->TraceRay( ray, MASK_SHOT_HULL | CONTENTS_GRATE, &filter, &pos_trace );

		// Third center ray from the head center to the local eye. Gives a
		// baseline for how occluded the direct line is.
		ray.Init( eye_pos, local_player->get_eye_pos() );
		interfaces::engine_trace()->TraceRay( ray, MASK_SHOT_HULL | CONTENTS_GRATE, &filter, &center_trace );

		if ( neg_trace.startsolid && pos_trace.startsolid )
			return 0;

		if ( neg_trace.startsolid )
			return -1;

		if ( pos_trace.startsolid )
			return 1;

		if ( neg_trace.fraction == 1.f && pos_trace.fraction == 1.f )
			return 0;

		// Fraction-based side selection. Fake-angle cheats put their real head
		// behind the wall, so if one side is heavily occluded (fraction < 0.2)
		// while the other is fully exposed (fraction > 0.8), resolve towards
		// the occluded side. The old code only compared which fraction was
		// smaller, which flipped on minor wall edges and thrashed the resolver.
		const auto neg_occluded = neg_trace.fraction < 0.2f;
		const auto pos_occluded = pos_trace.fraction < 0.2f;
		const auto neg_exposed = neg_trace.fraction > 0.8f;
		const auto pos_exposed = pos_trace.fraction > 0.8f;

		if ( neg_occluded && pos_exposed )
			return -1;

		if ( pos_occluded && neg_exposed )
			return 1;

		// Ambiguous (both similarly occluded or both similarly exposed) —
		// defer to desync delta / layer match instead of guessing.
		if ( !neg_exposed && !pos_exposed )
			return 0;

		// Both exposed but one side more blocked than the other — only commit
		// if the difference is significant (one side at least 2x more occluded).
		if ( neg_trace.fraction < pos_trace.fraction * 0.5f )
			return -1;

		if ( pos_trace.fraction < neg_trace.fraction * 0.5f )
			return 1;

		return 0;
	}

	// Adaptive state-aware bruteforce. Replaces the old `m_shots % 3` modulo
	// cycle. Instead of a fixed min/zero/max sequence, the candidate order is
	// picked from the enemy's classified anti-aim state and the candidate is
	// skipped if `get_brute_angle`'s damage-trace blacklist has already ruled
	// it out for this mode/side. The just-missed direction (`skip_dir`) is also
	// skipped so a confirmed resolve miss immediately pushes us to the next
	// candidate instead of re-trying the same angle.
	resolver_direction get_brute_direction( player_log_t& log, const int desync_side,
		const enemy_aa_state_t aa_state = enemy_aa_state_t::state_normal,
		const resolver_direction skip_dir = resolver_direction::resolver_invalid )
	{
		const auto mode = log.m_current_mode;
		const auto side = log.m_current_side;
		const auto& blacklist = log.m_mode[ mode ].m_side[ side ].m_blacklist;

		// Build the candidate order based on the classified state. "opposite"
		// means the side the desync is NOT on — try that first because the
		// resolver's initial guess is usually the desync side itself.
		const auto opposite = desync_side > 0 ? resolver_direction::resolver_min : resolver_direction::resolver_max;
		const auto same = desync_side > 0 ? resolver_direction::resolver_max : resolver_direction::resolver_min;

		std::array< resolver_direction, 3 > candidates = {
			opposite,
			resolver_direction::resolver_zero,
			same
		};

		switch ( aa_state )
		{
			case enemy_aa_state_t::state_jitter:
				// Jitter lives at the edges; zero catches the midpoint flick.
				candidates = { resolver_direction::resolver_zero, opposite, same };
				break;
			case enemy_aa_state_t::state_low_delta:
				// Low delta: the desync is small, so the head is near the
				// center. Zero first, then the two edges.
				candidates = { resolver_direction::resolver_zero, opposite, same };
				break;
			case enemy_aa_state_t::state_fake_flick:
				// Trust the server yaw first, then zero, then brute.
				candidates = { resolver_direction::resolver_networked, resolver_direction::resolver_zero, opposite };
				break;
			case enemy_aa_state_t::state_static:
			case enemy_aa_state_t::state_normal:
			default:
				// opposite -> zero -> same
				break;
		}

		// First pass: pick the first non-blacklisted, non-skipped candidate.
		for ( const auto dir : candidates )
		{
			if ( dir == skip_dir )
				continue;

			if ( dir != resolver_direction::resolver_networked && blacklist[ dir ] )
				continue;

			return dir;
		}

		// Everything blacklisted — fall back to networked and let
		// get_brute_angle reset the blacklist on the next miss.
		return resolver_direction::resolver_networked;
	}

	void reset_bruteforce_state( player_log_t& log )
	{
		log.m_shots = 0;
		log.m_stored_brute_yaw = 0.f;
		log.m_last_brute_mode = 0;
		log.m_desync_side = 0;
		log.m_extending = false;
		log.m_resolver_state = static_cast< int >( enemy_aa_state_t::state_normal );
		log.m_delay_ticks = 0;
		
		for ( auto& mode : log.m_mode )
		{
			for ( auto& side : mode.m_side )
			{
				side.m_blacklist = {};
				side.m_current_dir = resolver_direction::resolver_networked;
			}
		}
	}

	bool should_reset_bruteforce( player_log_t& log, const lag_record_t* current, const lag_record_t* previous )
	{
		if ( !current )
			return false;
		
		if ( current->m_dormant )
			return true;
		
		if ( previous )
		{
			const auto vel_diff = ( current->m_velocity - previous->m_velocity ).Length2D();
			if ( vel_diff > 50.f )
				return true;

			if ( is_fake_flick( current, previous ) )
				return true;
			
			if ( fabsf( current->m_lby - previous->m_lby ) > 1.f )
				return true;
		}
		
		return false;
	}

	resolver_direction resolve_shot_direction( C_CSPlayer* player, lag_record_t* record )
	{
		if ( !local_player )
			return resolver_direction::resolver_networked;

		const auto& log = player_log::get_log( record->m_index );
		const auto rotation = log.m_extending ? 58.f : 29.f;
		const auto eye_yaw = record->m_eye_angles.y;
		const auto fire_yaw = math::normalize_float( math::calc_angle( player->get_eye_pos(), local_player->get_eye_pos() ).y );

		const auto left_delta = fabsf( math::normalize_float( fire_yaw - ( eye_yaw + rotation ) ) );
		const auto right_delta = fabsf( math::normalize_float( fire_yaw - ( eye_yaw - rotation ) ) );

		return left_delta > right_delta ? resolver_direction::resolver_min : resolver_direction::resolver_max;
	}

	resolver_direction match_simulated_layers( C_CSPlayer* player, lag_record_t* record, const lag_record_t* previous )
	{
		if ( !previous )
			return resolver_direction::resolver_networked;

		const auto speed = record->m_velocity.Length2D();
		// Velocity floor: below 1.1 u/s the movement-weight layer is too noisy
		// to be a reliable signal. The old 0.1f floor let near-stationary
		// fake-walkers through and produced confident-wrong picks that caused
		// mag dumps. Defer to freestanding / LBY delta in this case.
		if ( speed <= 1.1f )
			return resolver_direction::resolver_networked;

		const bool transitioning = record->m_layers[ 12 ].m_flWeight > 0.01f;
		const bool constant_speed = fabsf( record->m_layers[ 6 ].m_flWeight - previous->m_layers[ 6 ].m_flWeight ) < 0.01f;

		if ( transitioning || !constant_speed )
			return resolver_direction::resolver_networked;

		// Weighted multi-layer score. Relying on a single layer (the old code
		// only compared layer 6 playback rate) is vulnerable to speed spoofing
		// and pitch-based animstate manipulation. We now combine three layers:
		//   - Layer 6 (movement/weight): primary signal, weight 1.0
		//   - Layer 11 (jump/fall/crouch activity): weight 0.6, weight is more
		//     stable than playback for this layer
		//   - Layer 3 (body lean/cycle): weight 0.4
		// Lowest cumulative weighted delta wins, subject to a confidence gate.
		struct layer_sig
		{
			int index;
			float weight;
			enum field_t { field_playback, field_weight, field_cycle } field;
		};
		constexpr std::array< layer_sig, 3 > layers = { {
			{ 6,  1.0f, layer_sig::field_playback },
			{ 11, 0.6f, layer_sig::field_weight   },
			{ 3,  0.4f, layer_sig::field_cycle    },
		} };

		auto get_layer_value = [] ( const std::array< C_AnimationLayer, 13 >& arr, const layer_sig& sig ) -> float
		{
			switch ( sig.field )
			{
				case layer_sig::field_playback: return arr[ sig.index ].m_flPlaybackRate;
				case layer_sig::field_weight:   return arr[ sig.index ].m_flWeight;
				case layer_sig::field_cycle:    return arr[ sig.index ].m_flCycle;
			}
			return 0.f;
		};

		auto best_dir = resolver_direction::resolver_networked;
		auto best_score = FLT_MAX;
		auto runner_up = FLT_MAX;

		for ( int i = static_cast< int >( resolver_direction::resolver_networked ); i < static_cast< int >( resolver_direction::resolver_direction_max ); i++ )
		{
			const auto dir = static_cast< resolver_direction >( i );
			auto score = 0.f;

			for ( const auto& sig : layers )
			{
				const auto net_val = get_layer_value( record->m_layers, sig );
				const auto sim_val = get_layer_value( record->m_state[ dir ].m_own_layers, sig );
				score += sig.weight * fabsf( net_val - sim_val );
			}

			if ( score < best_score )
			{
				runner_up = best_score;
				best_score = score;
				best_dir = dir;
			}
			else if ( score < runner_up )
			{
				runner_up = score;
			}
		}

		// Confidence gate. Only accept the best direction if it is clearly
		// better than the runner-up (score < 70% of runner-up) AND below an
		// absolute threshold derived from the old 0.001f playback delta
		// (scaled up by the number of weighted layers). Otherwise defer to
		// the networked yaw so we don't commit to a confident-wrong pick.
		constexpr auto absolute_threshold = 0.0035f;
		if ( best_score < absolute_threshold && ( runner_up == FLT_MAX || best_score < runner_up * 0.7f ) )
			return best_dir;

		return resolver_direction::resolver_networked;
	}
}

void resolver::resolve( C_CSPlayer* player, lag_record_t* record, lag_record_t* previous )
{
	if ( !player->is_enemy() )
		return;

	auto& log = player_log::get_log( record->m_index );

	if ( should_reset_bruteforce( log, record, previous ) )
		reset_bruteforce_state( log );

	log.player = player;
	log.m_extending = is_extended_desync( record );

	// Detect and log player state
	resolver_state_t state = resolver_state_t::STATE_STANDING;
	const auto speed = record->m_velocity.Length2D();
	const auto in_air = !( record->m_flags & FL_ONGROUND ) || player->get_move_type() == MOVETYPE_LADDER;

	if ( in_air )
		state = resolver_state_t::STATE_IN_AIR;
	else if ( record->m_duckamt > 0.55f )
		state = speed > 1.1f ? resolver_state_t::STATE_CROUCH_MOVING : resolver_state_t::STATE_CROUCHING;
	else if ( speed > 1.1f )
		state = speed > 130.f ? resolver_state_t::STATE_RUNNING : resolver_state_t::STATE_WALKING;
	else
		state = resolver_state_t::STATE_STANDING;

	// Cache the anti-aim classification for this tick so apply_desync_side
	// and get_brute_direction see the same verdict.
	const auto aa_state = classify_enemy_state( log, record, previous );
	log.m_resolver_state = static_cast< int >( aa_state );

#ifdef _DEBUG
	interfaces::cvar()->ConsoleColorPrintf( Color( 0, 255, 120, 255 ), "[purple] Enemy %s index %d state detected: %s | aa: %s\n", 
		player->get_player_info().szName, record->m_index, state_names[ static_cast<int>(state) ], aa_state_names[ static_cast<int>(aa_state) ] );
#endif

	if ( !record->m_shot )
	{
		if ( !is_fake_flick( record, previous ) )
			log.m_last_non_shot_angles = record->m_eye_angles;

		pitch_resolve( record );
	}
	else if ( log.m_unknown_shot )
		log.m_mode[ resolver_mode::resolver_shot ].m_side[ log.m_current_side ].m_current_dir = resolve_shot_direction( player, record );

	yaw_resolve( record, previous );
	apply_desync_side( player, record, previous );
}

void resolver::apply_desync_side( C_CSPlayer* player, lag_record_t* record, const lag_record_t* previous )
{
	if ( record->m_shot )
		return;

	auto& log = player_log::get_log( record->m_index );
	auto& current_dir = log.m_mode[ log.m_current_mode ].m_side[ log.m_current_side ].m_current_dir;

	const auto speed = record->m_velocity.Length2D();
	const auto in_air = !( record->m_flags & FL_ONGROUND ) || player->get_move_type() == MOVETYPE_LADDER;
	const auto crouching = record->m_duckamt > .55f;
	const auto standing = speed <= 1.1f;
	const auto aa_state = static_cast< enemy_aa_state_t >( log.m_resolver_state );

	// Defensive records (breaking LC, tickbase shift, extrapolated, heavy
	// lag, simtime regress) have unreliable layer/foot_yaw data. Trust the
	// networked yaw instead of bruting — bruting on garbage data wastes
	// shots and pollutes the blacklist with false negatives.
	if ( is_defensive_tick( record, previous ) )
	{
		current_dir = resolver_direction::resolver_networked;
		return;
	}

	// LBY break: the enemy flicked their lower body yaw. The desync resets
	// to the LBY side for the next ~0.22s. Detected here so the standing
	// branch can follow the break instead of trusting the previous desync.
	const auto breaking_lby = is_breaking_lby( record, previous );

	if ( in_air )
	{
		if ( log.m_shots == 0 )
		{
			// In air, foot yaw follows the velocity direction (the body
			// aligns to the movement tangent). Use that as the desync side
			// when we have enough velocity to compute a tangent; otherwise
			// fall back to the networked yaw. Air desync is constrained
			// (~25°), so the tangent is a strong first guess.
			if ( record->m_velocity.Length2D() > 5.f )
			{
				QAngle vel_ang;
				math::vector_angles( record->m_velocity, vel_ang );
				const auto vel_yaw = math::normalize_float( vel_ang.y );
				const auto desync_delta = math::angle_diff( record->m_eye_angles.y, vel_yaw );
				log.m_desync_side = desync_delta <= 0.f ? 1 : -1;
				current_dir = side_to_direction( log.m_desync_side );
			}
			else
				current_dir = resolver_direction::resolver_networked;
		}
		else
		{
			// Adaptive brute for air targets. Air desync is constrained so the
			// candidate order from get_brute_direction still applies; we just
			// feed it the cached aa_state.
			current_dir = get_brute_direction( log, log.m_desync_side != 0 ? log.m_desync_side : 1, aa_state );
		}
		return;
	}

	const auto freestand_side = detect_freestand( player, record, log );
	const auto foot_yaw = record->m_state[ resolver_direction::resolver_networked ].m_animstate.foot_yaw;

	if ( freestand_side < 0 )
		log.m_current_side = resolver_side::resolver_left;
	else if ( freestand_side > 0 )
		log.m_current_side = resolver_side::resolver_right;

	if ( freestand_side != 0 )
		log.m_desync_side = freestand_side;
	else
	{
		const auto desync_delta = math::angle_diff( record->m_eye_angles.y, foot_yaw );
		if ( fabsf( desync_delta ) > 1.f )
			log.m_desync_side = desync_delta <= 0.f ? 1 : -1;
		else
		{
			const auto forward_yaw = get_forward_yaw( player );
			log.m_desync_side = math::angle_diff( record->m_eye_angles.y, forward_yaw ) > 0.f ? -1 : 1;
		}
	}

	refresh_low_delta_state( log, record, foot_yaw );

	// Resolve standing & crouching players
	if ( standing || crouching )
	{
		// LBY break: follow the break direction. The desync resets to the
		// LBY side, so override the desync side and resolve directly.
		if ( breaking_lby )
		{
			const auto lby_break_delta = math::angle_diff( record->m_lby, previous->m_lby );
			log.m_desync_side = lby_break_delta <= 0.f ? 1 : -1;
			current_dir = side_to_direction( log.m_desync_side );
		}
		// |Delta| > 35° standing rule: when the eye-to-foot delta is large,
		// the head is committed to the desync edge. Skip freestand/layer
		// matching (they're unreliable for standing targets with committed
		// desync) and resolve directly to the desync side. Only on the
		// first shot; after a miss, the adaptive brute takes over.
		else if ( log.m_shots == 0 && fabsf( math::angle_diff( record->m_eye_angles.y, foot_yaw ) ) > 35.f )
		{
			current_dir = side_to_direction( log.m_desync_side );
		}
		else if ( log.m_shots > 0 )
		{
			current_dir = get_brute_direction( log, log.m_desync_side != 0 ? log.m_desync_side : 1, aa_state );
		}
		else if ( freestand_side != 0 )
		{
			current_dir = side_to_direction( freestand_side );
		}
		else
		{
			// Per-state initial pick. For low-delta and jitter, the head is
			// near the center, so zero is the best first guess. For fake-
			// flick, trust the networked yaw first. For static/normal, use
			// the layer match then the desync side fallback.
			switch ( aa_state )
			{
				case enemy_aa_state_t::state_low_delta:
				case enemy_aa_state_t::state_jitter:
					current_dir = resolver_direction::resolver_zero;
					break;
				case enemy_aa_state_t::state_fake_flick:
					current_dir = resolver_direction::resolver_networked;
					break;
				default:
				{
					const auto layer_dir = match_simulated_layers( player, record, previous );
					if ( layer_dir != resolver_direction::resolver_networked )
						current_dir = layer_dir;
					else
						current_dir = side_to_direction( log.m_desync_side );
					break;
				}
			}
		}

#ifdef _DEBUG
		interfaces::cvar()->ConsoleColorPrintf( Color( 120, 200, 255, 255 ), "[purple] resolve %d [%s] dir=%d shots=%d side=%d lby=%d\n",
			record->m_index, aa_state_names[ static_cast<int>(aa_state) ], static_cast<int>(current_dir), log.m_shots, log.m_desync_side, (int)breaking_lby );
#endif
		return;
	}

	// Resolve moving players using simulated animation layers
	const auto layer_dir = match_simulated_layers( player, record, previous );
	if ( layer_dir != resolver_direction::resolver_networked )
	{
		current_dir = layer_dir;
	}
	else
	{
		// Moving + no confident layer match: if we've missed, use the adaptive
		// brute instead of blindly trusting the networked yaw. This is what
		// lets us recover against moving desyncers that spoof layer 6.
		if ( log.m_shots > 0 )
			current_dir = get_brute_direction( log, log.m_desync_side != 0 ? log.m_desync_side : 1, aa_state );
		else
			current_dir = resolver_direction::resolver_networked;
	}

#ifdef _DEBUG
	interfaces::cvar()->ConsoleColorPrintf( Color( 120, 200, 255, 255 ), "[purple] resolve(moving) %d [%s] dir=%d shots=%d side=%d\n",
		record->m_index, aa_state_names[ static_cast<int>(aa_state) ], static_cast<int>(current_dir), log.m_shots, log.m_desync_side );
#endif
}

void resolver::post_animate( C_CSPlayer* player, lag_record_t* record )
{
	const auto log = &player_log::get_log( player->EntIndex() );

	if ( !player->is_enemy() || player->get_player_info().fakeplayer )
		log->m_mode[ resolver_mode::resolver_shot ].m_side = log->m_mode[ resolver_mode::resolver_default ].m_side = log->m_mode[ resolver_mode::resolver_flip ].m_side = {};

	record->m_resolver_mode = record->m_shot ? resolver_mode::resolver_shot : log->m_current_mode;
	record->m_resolver_side = log->m_current_side;

	if ( !record->m_shot )
	{
		const auto cureye = record->m_eye_angles;
		if ( fabsf( cureye.x ) >= 60.f )
			log->m_last_unusual_pitch = interfaces::globals()->curtime;
		else
			log->m_last_zero_pitch = interfaces::globals()->curtime;
	}

	if ( log->m_unknown_shot && log->m_mode[ log->m_current_mode ].m_side[ log->m_current_side ].m_current_dir > resolver_direction::resolver_networked )
		log->m_mode[ resolver_mode::resolver_shot ].m_side[ log->m_current_side ].m_current_dir = log->m_mode[ log->m_current_mode ].m_side[ log->m_current_side ].m_current_dir;
}

bool resolver::extrapolate_record( int ticks, lag_record_t& outrecord, const bool simple )
{
	if ( !ticks )
	{
		outrecord.setup_matrices();
		return true;
	}

	const auto player = globals::get_player( outrecord.m_index );

	const auto backup_lby = player->get_lby();
	const auto backup_layers = player->get_anim_layers();
	const auto backup_state = *player->get_anim_state();
	const auto backup_poses = player->get_pose_params();
	const auto backup_angle = player->get_abs_rotation();

	const auto backup_abs_origin = player->get_abs_origin();
	const auto backup_flags = player->get_flags();
	const auto backup_groundentity = player->get_ground_entity();
	const auto backup_move_type = player->get_move_type();
	const auto backup_velocity = player->get_velocity();
	const auto backup_ducking = player->get_ducking();

	outrecord.m_velocity = outrecord.m_calculated_velocity;

	player->get_velocity().z = outrecord.m_calculated_velocity.z;

	auto new_previous = std::make_unique<lag_record_t>();
	*new_previous = outrecord;
	new_previous->m_extrapolated = true;
	auto& log = player_log::get_log( outrecord.m_index );

	if ( simple )
	{
		process_move_changes_t backup_pm{};
		backup_pm.store( player );

		const auto original_record = log.record.back();
		const auto p1 = log.record.size() > 1 ? &log.record[ log.record.size() - 2 ] : nullptr;
		const auto p2 = log.record.size() > 2 ? &log.record[ log.record.size() - 3 ] : nullptr;

		int prev_buttons = 0;

		Vector predicted_vel_change{}, record_vel_change{};
		if ( p1 && p2 )
		{
			const auto p1_vel_change = ( p1->m_calculated_velocity - p2->m_calculated_velocity ) / p1->m_lagamt;
			record_vel_change = ( original_record.m_calculated_velocity - p1->m_calculated_velocity ) / original_record.m_lagamt;
			predicted_vel_change = record_vel_change - p1_vel_change;
		}

		const auto speed = original_record.m_velocity.Length2D();

		CUserCmd cmd{};
		for ( auto i = 0; i < ticks; i++ )
		{
			QAngle predicted_vel_change_ang;
			math::vector_angles( player->get_velocity() + predicted_vel_change, predicted_vel_change_ang );
			cmd.viewangles.y = predicted_vel_change_ang.y;
			cmd.viewangles.x = 0;

			cmd.forwardmove = speed > 5.f ? 450.f : ( i % 2 ? 1.01f : -1.01f );
			cmd.sidemove = 0.f;

			if ( original_record.m_duckamt > 0.f )
				cmd.buttons |= IN_DUCK;
			else
				cmd.buttons &= ~IN_DUCK;

			if ( i == 0 )
			{
				if ( player->get_duck_amt() > 0.f )
					player->get_ducking() = true;
				else
					player->get_ducking() = false;

				if ( player->get_duck_amt() == 1.f )
				{
					player->get_ducked() = true;
					player->get_ducking() = false;
				}
				else
					player->get_ducked() = false;

				prev_buttons = cmd.buttons;

				if ( !( player->get_flags() & FL_ONGROUND ) )
					prev_buttons |= IN_JUMP;
			}

			if ( !( player->get_flags() & FL_ONGROUND ) )
			{
				QAngle vel_ang;
				math::vector_angles( player->get_velocity(), vel_ang );

				if ( fabsf( math::normalize_float( vel_ang.y - predicted_vel_change_ang.y ) ) > 20.f )
				{
					cmd.forwardmove = 0.f;
					cmd.sidemove = fabsf( vel_ang.y - predicted_vel_change_ang.y ) > 0.f ? 450.f : -450.f;
				}
			}
			else if ( p1 && speed < p1->m_velocity.Length2D() - 5.f * original_record.m_lagamt || speed < 5.f )
			{
				CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
				aimbot_helpers::stop_to_speed( 1.01f, &data, player );
				cmd.forwardmove = data.m_flForwardMove;
				cmd.sidemove = data.m_flSideMove;
			}
			else if ( p1 && speed < p1->m_velocity.Length2D() + 5.f * original_record.m_lagamt && speed > 5.f )
			{
				CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
				aimbot_helpers::stop_to_speed( ( player->get_velocity() + predicted_vel_change ).Length2D(), &data, player );
				cmd.forwardmove = data.m_flForwardMove;
				cmd.sidemove = data.m_flSideMove;
			}

			CMoveData data = interfaces::game_movement()->setup_move( player, &cmd );
			data.m_nOldButtons = prev_buttons;
			const auto ret = interfaces::game_movement()->process_movement( player, &data );
			prev_buttons = data.m_nButtons;
			ret.restore( player );

			if ( p1 )
			{
				if ( !( p1->m_flags & FL_ONGROUND ) && !( original_record.m_flags & FL_ONGROUND ) && player->get_flags() & FL_ONGROUND )
					cmd.buttons |= IN_JUMP;
				else
					cmd.buttons &= ~IN_JUMP;
			}

			player->set_abs_origin( data.m_vecAbsOrigin );
			player->get_velocity() = data.m_vecVelocity;

			if ( i == ticks - 1 )
				outrecord.m_origin = data.m_vecAbsOrigin;
		}

		backup_pm.restore( player );
		player->set_abs_origin( backup_abs_origin );
		player->get_flags() = backup_flags;
		player->get_ground_entity() = backup_groundentity;
		player->get_move_type() = backup_move_type;
		player->get_velocity() = backup_velocity;
		player->get_ducking() = backup_ducking;

		return true;
	}

	new_previous->m_velocity = outrecord.m_calculated_velocity;
	outrecord.m_simtime += interfaces::globals()->interval_per_tick * ticks;
	outrecord.m_lagamt = ticks;
	animations::update_player_animations( &outrecord, player, new_previous.get() );

	player->get_lby() = backup_lby;
	player->get_anim_layers() = backup_layers;
	*player->get_anim_state() = backup_state;
	player->get_pose_params() = backup_poses;
	player->set_abs_angles( backup_angle );
	player->get_velocity() = backup_velocity;

	for ( auto& state : outrecord.m_state )
		state.m_setup_tick = -1;
	outrecord.setup_matrices( resolver_direction::resolver_invalid, true );

	return true;
}

void resolver::pitch_resolve( lag_record_t* record )
{
	const auto& log = player_log::get_log( record->m_index );

	if ( globals::nospread )
	{
		if ( log.nospread.m_pitch_cycle % 2 && log.nospread.m_can_fake )
		{
			record->m_eye_angles.x = -record->m_eye_angles.x;
		}
	}

	record->m_pitch_cycle = log.nospread.m_pitch_cycle;
}

float resolver::get_resolver_angle( const lag_record_t& record, resolver_direction direction, float eye_angle )
{
	const auto base_foot_yaw = record.m_state[ resolver_direction::resolver_networked ].m_animstate.foot_yaw;

	switch ( direction )
	{
		case resolver_direction::resolver_max:
			return build_server_abs_yaw( record, math::normalize_float( eye_angle + get_direction_delta( record, direction ) ), base_foot_yaw );
		case resolver_direction::resolver_min:
			return build_server_abs_yaw( record, math::normalize_float( eye_angle + get_direction_delta( record, direction ) ), base_foot_yaw );
		case resolver_direction::resolver_zero:
		case resolver_direction::resolver_networked:
			return build_server_abs_yaw( record, eye_angle, base_foot_yaw );
		default:
			return eye_angle;
	}
}

void resolver::yaw_resolve( const lag_record_t* record, const lag_record_t* previous )
{
	if ( record->m_shot || ( previous && previous->m_shot ) )
		return;

	auto& log = player_log::get_log( record->m_index );

	const auto delta = !previous ? 0.f : math::normalize_float( record->m_eye_angles.y - previous->m_eye_angles.y );

	// State-aware flip threshold. Jitter naturally produces large deltas
	// (>35° swings), so a 30° threshold false-positives on every jitter
	// tick and thrashes the mode. Static AA produces small deltas, so a
	// lower threshold catches real flips that 30° would miss. Low-delta
	// sits between. Fake-flick is handled by classify_enemy_state and the
	// per-state brute, not by the flip detector.
	const auto aa_state = static_cast< enemy_aa_state_t >( log.m_resolver_state );
	const auto flip_threshold = aa_state == enemy_aa_state_t::state_jitter ? 55.f
		: aa_state == enemy_aa_state_t::state_static ? 15.f
		: aa_state == enemy_aa_state_t::state_low_delta ? 25.f
		: 30.f;

	const auto use_nonflip = delta < -flip_threshold;
	const auto use_flip = delta > flip_threshold;

	const auto previous_mode = log.m_current_mode;

	if ( ( log.m_current_mode == resolver_mode::resolver_flip && use_nonflip || log.m_current_mode == resolver_mode::resolver_default && use_flip ) )
		log.m_current_mode = static_cast< resolver_mode >( !static_cast< int >( log.m_current_mode ) );
	else if ( fabsf( delta ) > 170.f )
		log.m_current_mode = static_cast< resolver_mode >( !static_cast< int >( log.m_current_mode ) );

	if ( previous_mode != log.m_current_mode )
		log.m_last_flip_tick = interfaces::client_state()->get_last_server_tick();

	if ( interfaces::client_state()->get_last_server_tick() - log.m_last_flip_tick > time_to_ticks( 1.1f ) )
		log.m_current_mode = resolver_mode::resolver_default;
}

void resolver::update_moving_side( lag_record_t* record, const lag_record_t* previous )
{
	if ( !previous || record->m_shot || record->m_velocity.Length2D() <= 4.f )
		return;

	if ( !( record->m_flags & FL_ONGROUND ) )
		return;

	auto& log = player_log::get_log( record->m_index );

	if ( !log.m_unknown )
		return;

	const auto aa_state = static_cast< enemy_aa_state_t >( log.m_resolver_state );
	log.m_mode[ log.m_current_mode ].m_side[ log.m_current_side ].m_current_dir = get_brute_direction( log, log.m_desync_side, aa_state );
}

void resolver::on_createmove()
{
	if ( tickbase::force_choke )
		return;

	std::vector<std::shared_ptr<detail::call_queue::queue_element>> calls;

	static Vector last_eyepos = {};
	const auto eyepos = local_player->get_eye_pos();

	for ( const auto player : interfaces::entity_list()->get_players() )
	{
		auto& log = player_log::get_log( player->EntIndex() );
		if ( player->IsDormant() || !player->is_enemy() || log.record.empty() || player->get_player_info().fakeplayer || !log.is_hittable )
			continue;

		auto& newest = log.record.back();

		if ( fabsf( eyepos.Dist( last_eyepos ) ) > 2.f )
			newest.m_did_wall_detect = false;

		if ( newest.m_did_wall_detect )
			continue;

		wall_detect( &newest );
	}

	last_eyepos = eyepos;
}

void resolver::wall_detect( lag_record_t* record )
{
	auto& log = player_log::get_log( record->m_index );

	const auto should_change_desync = !record->m_shot && log.m_unknown && !record->m_did_wall_detect && record->m_lagamt >= 1;

	const auto weapon = local_weapon;
	if ( !weapon || !weapon->is_gun() )
		return;

	const auto player = globals::get_player( record->m_index );

	if ( !is_sideways( player, record ) )
		return;

	const auto hdr = player->get_model_ptr();
	if ( !hdr )
		return;

	const auto studio_hdr = hdr->m_pStudioHdr;
	if ( !studio_hdr )
		return;

	const auto hitbox_set = studio_hdr->pHitboxSet( player->get_hitbox_set() );
	if ( !hitbox_set )
		return;

	const auto hitbox = hitbox_set->pHitbox( HITBOX_HEAD );
	if ( !hitbox )
		return;

	auto get_rotated_pos = [] ( Vector start, const float rotation, const float distance )
	{
		const auto rad = DEG2RAD( rotation );
		start.x += cos( rad ) * distance;
		start.y += sin( rad ) * distance;

		return start;
	};

	const auto eye_pos = record->m_origin + Vector( 0.f, 0.f, 60.f );
	const auto target_position = current_eye;
	const auto target_angle = math::calc_angle( eye_pos, target_position );

	const auto weapon_info = interfaces::weapon_system()->GetWpnData( WEAPON_AWP );

	const auto local_pos_left = get_rotated_pos( eye_pos, math::normalize_float( target_angle.y - 90.f ), 25.f );
	const auto local_pos_right = get_rotated_pos( eye_pos, math::normalize_float( target_angle.y + 90.f ), 25.f );

	const auto local_half_pos_left = get_rotated_pos( eye_pos, math::normalize_float( target_angle.y - 90.f ), 12.f );
	const auto local_half_pos_right = get_rotated_pos( eye_pos, math::normalize_float( target_angle.y + 90.f ), 12.f );

	const auto enemy_pos_left = get_rotated_pos( target_position, math::normalize_float( target_angle.y - 90.f ), 25.f );
	const auto enemy_pos_right = get_rotated_pos( target_position, math::normalize_float( target_angle.y + 90.f ), 25.f );

	const auto compare = [&player, &weapon_info] ( const Vector& from_left, const Vector& from_right, const Vector& left, const Vector& right, const bool check = false ) -> int
	{
		auto pen_weapon = *weapon_info;
		if ( !check )
			pen_weapon.idamage = 200;

		aimbot::aimpoint_t aimpoint_left{};
		aimpoint_left.point = left;
		can_hit( player, penetration::pen_data( {}, {}, {}, {}, &pen_weapon ), from_left, &aimpoint_left, aimpoint_left.damage );

		if ( check )
			return aimpoint_left.damage > 0;

		aimbot::aimpoint_t aimpoint_right{};
		aimpoint_right.point = right;
		can_hit( player, penetration::pen_data( {}, {}, {}, {}, &pen_weapon ), from_right, &aimpoint_right, aimpoint_right.damage );

		if ( !aimpoint_left.damage && aimpoint_right.damage )
			return 1;

		if ( !aimpoint_right.damage && aimpoint_left.damage )
			return 2;

		return 0;
	};

	auto goal_dir = -1;

	if ( const auto res = compare( local_pos_left, local_pos_right, enemy_pos_left, enemy_pos_right ); res && !compare( eye_pos, eye_pos, res == 1 ? enemy_pos_left : enemy_pos_right, enemy_pos_right, true ) )
	{
		goal_dir = res == 1 ? 1 : 2;
	}
	else if ( const auto res = compare( local_pos_left, local_pos_right, enemy_pos_right, enemy_pos_left ); res && !compare( eye_pos, eye_pos, res == 1 ? enemy_pos_left : enemy_pos_right, enemy_pos_right, true ) )
	{
		goal_dir = res == 1 ? 1 : 2;
	}

	if ( goal_dir != -1 && compare( goal_dir == 1 ? local_half_pos_left : local_half_pos_right, target_position, target_position, target_position, true ) )
	{
		goal_dir = -1;
	}

	if ( goal_dir == -1 )
	{
		// Mark as detected even on failure so we don't re-run the (expensive)
		// wall detection every tick against sideways peekers and thrash the
		// resolver. The flag is cleared in on_createmove when our eye moves.
		record->m_did_wall_detect = true;
		return;
	}

	record->m_did_wall_detect = true;

	log.m_wall_detect_ang = math::normalize_float( target_angle.y + ( goal_dir == 1 ? -90.f : 90.f ) );

	auto closest_state = resolver_direction::resolver_invalid;

	if ( should_change_desync )
	{
		record->setup_matrices();

		auto closest_angle = FLT_MAX;
		for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
		{
			auto& state = record->m_state[ i ];

			const auto pos = Vector( state.m_matrix[ hitbox->bone ][ 0 ][ 3 ], state.m_matrix[ hitbox->bone ][ 1 ][ 3 ], state.m_matrix[ hitbox->bone ][ 2 ][ 3 ] );
			const auto angle = math::calc_angle( record->m_origin, pos );
			const auto diff = fabsf( math::normalize_float( angle.y - log.m_wall_detect_ang ) );
			if ( diff < closest_angle )
			{
				closest_angle = diff;
				closest_state = i;
			}
		}
	}

	log.m_current_side = goal_dir == 1 ? resolver_side::resolver_left : resolver_side::resolver_right;

	if ( closest_state != resolver_direction::resolver_invalid )
		log.m_mode[ log.m_current_mode ].m_side[ log.m_current_side ].m_current_dir = closest_state;
}

void resolver::add_shot( shot_t& shot )
{
	shots.emplace_back( shot );
}

void resolver::update_missed_shots( const ClientFrameStage_t& stage )
{
	if ( stage != FRAME_NET_UPDATE_END )
		return;

	auto it = shots.begin();
	while ( it != shots.end() )
	{
		const auto shot = *it;
		if ( shot.tick + time_to_ticks( 1.f ) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount )
		{
			it = shots.erase( it );
		}
		else
		{
			++it;
		}
	}

	auto it2 = current_shots.begin();
	while ( it2 != current_shots.end() )
	{
		const auto shot = *it2;
		if ( shot.tick + time_to_ticks( 1.f ) < interfaces::globals()->tickcount || shot.tick - 10 > interfaces::globals()->tickcount )
		{
			it2 = current_shots.erase( it2 );
		}
		else
		{
			++it2;
		}
	}
}

void resolver::hurt_listener( IGameEvent* game_event, record_shot_info_t& shot_info )
{
	const auto attacker = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "attacker" ) );
	const auto victim = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid"  ) );
	const auto hitgroup = game_event->GetInt( "hitgroup" );
	const auto damage = game_event->GetInt( "dmg_health" );

	if ( attacker != interfaces::engine()->GetLocalPlayer() )
		return;

	if ( victim == interfaces::engine()->GetLocalPlayer() )
		return;

	const auto player = globals::get_player( victim );
	if ( !player || !player->is_enemy() )
		return;

	if ( unapproved_shots.empty() )
		return;

	for ( auto& shot : unapproved_shots )
	{
		if ( !shot.hurt && shot.enemy_index == victim )
		{
			shot.hurt = true;
			shot.hitinfo.victim = victim;
			shot.hitinfo.hitgroup = hitgroup;
			shot.hitinfo.damage = damage;
			shot_info = shot.record.m_shot_info;
			return;
		}
	}
}

resolver::shot_t* resolver::closest_shot( int tickcount )
{
	shot_t* closest_shot = nullptr;
	for ( auto& shot : shots )
	{
		closest_shot = &shot;
		break;
	}

	return closest_shot;
}

bool resolver::record_shot( IGameEvent* game_event )
{
	const auto userid = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid" ) );
	const auto player = globals::get_player( userid );

	if ( player != local_player )
		return false;

	const auto shot = closest_shot( interfaces::globals()->tickcount - time_to_ticks( interfaces::engine()->GetNetChannelInfo()->GetLatency( FLOW_OUTGOING ) ) );
	if ( !shot )
		return false;

	current_shots.push_front( *shot );
	shots.pop_front();
	current_hitposes.clear();

	return true;
}

void resolver::listener( IGameEvent* game_event )
{
	static auto last_tickcount = 0;

	if ( !strcmp( game_event->GetName(), "weapon_fire" ) )
	{
		if ( record_shot( game_event ) )
			last_tickcount = 0;
		return;
	}

	if ( current_shots.empty() )
		return;

	const auto userid = interfaces::engine()->GetPlayerForUserID( game_event->GetInt( "userid"  ) );
	const auto player = globals::get_player( userid );

	if ( !player || player != local_player )
		return;

	const Vector pos( game_event->GetFloat( "x" ), game_event->GetFloat( "y" ), game_event->GetFloat( "z" ) );

	const auto shot = &current_shots[ 0 ];

	static auto counter = 0;

	if ( last_tickcount == interfaces::globals()->tickcount )
		counter++;
	else
	{
		current_hitposes.clear();
		counter = 0;
	}

	if ( counter )
		unapproved_shots.pop_front();

	current_hitposes.push_back( pos );
	shot->hitposes = current_hitposes;
	unapproved_shots.emplace_back( *shot );

	last_tickcount = interfaces::globals()->tickcount;
}

Vector resolver::get_closest_hitpos( const shot_t& shot, const Vector& pos )
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for ( auto& hitpos : shot.hitposes )
	{
		const auto dist = hitpos.Dist( pos );
		if ( dist < last_dist )
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

Vector resolver::get_closest_penetrationpos( const shot_t& shot, const Vector& pos )
{
	Vector closest = {};
	auto last_dist = FLT_MAX;
	for ( auto& hitpos : shot.penetration_points )
	{
		const auto dist = hitpos.Dist( pos );
		if ( dist < last_dist )
		{
			last_dist = dist;
			closest = hitpos;
		}
	}

	return closest;
}

void resolver::approve_shots( const ClientFrameStage_t& stage )
{
	if ( stage != FRAME_NET_UPDATE_END )
		return;

	for ( auto& shot : unapproved_shots )
	{
		if ( shot.hitposes.empty() )
			continue;

		auto end = shot.hitposes[ shot.hitposes.size() - 1 ];

		if ( vars::misc.impacts->get<bool>() )
		{
			auto col2 = Color( vars::misc.impacts_color2->get<D3DCOLOR>() );

			for ( auto& point : shot.hitposes )
				interfaces::debug_overlay()->AddBoxOverlay( point, Vector( -1.25f, -1.25f, -1.25f ), Vector( 1.25f, 1.25f, 1.25f ), QAngle( 0, 0, 0 ), col2.r(), col2.g(), col2.b(), 180, 4 );
		}

		if ( local_player && local_player->get_alive() && prediction::get_pred_info( shot.cmdnum ).sequence == shot.cmdnum )
		{
			auto new_origin = prediction::get_pred_info( shot.cmdnum ).origin;
			shot.shotpos.x = new_origin.x;
			shot.shotpos.y = new_origin.y;
		}

		const auto angles = math::calc_angle( shot.shotpos, end );
		Vector direction{};
		math::angle_vectors( angles, &direction );

		if ( shot.record.m_index == -1 )
		{
			if ( shot.hurt )
			{
				if ( shot.penetration_points.empty() )
					continue;

				shot.hitpos = get_closest_hitpos( shot, shot.penetration_points[ shot.penetration_points.size() - 1 ] );
			}

			Vector zerovec = {};
			lua::api.callback( FNV1A( "on_shot_registered" ), [&] ( lua::state& state )
			{
				state.create_table();
				state.set_field( XOR_STR( "manual" ), true );
				state.set_field( XOR_STR( "secure" ), false );
				state.set_field( XOR_STR( "very_secure" ), false );
				state.set_field( XOR_STR( "result" ), shot.hurt ? XOR_STR( "hit" ) : XOR_STR( "miss" ) );
				state.set_field( XOR_STR( "target" ), -1 );
				state.set_field( XOR_STR( "tick" ), shot.tick );
				state.set_field( XOR_STR( "backtrack" ), 0 );
				state.set_field( XOR_STR( "hitchance" ), -1 );
				state.set_field( XOR_STR( "client_hitgroup" ), -1 );
				state.set_field( XOR_STR( "client_damage" ), -1 );
				state.set_field( XOR_STR( "server_hitgroup" ), shot.hitinfo.hitgroup );
				state.set_field( XOR_STR( "server_damage" ), shot.hitinfo.damage );
				state.create_user_object<decltype( shot.shotpos )>( XOR_STR( "vec3" ), &shot.shotpos );
				state.set_field( XOR_STR( "shotpos" ) );
				state.create_user_object<decltype( zerovec )>( XOR_STR( "vec3" ), &zerovec );
				state.set_field( XOR_STR( "client_hitpos" ) );
				state.create_user_object<decltype( shot.hitpos )>( XOR_STR( "vec3" ), shot.hurt ? &shot.hitpos : &zerovec );
				state.set_field( XOR_STR( "server_hitpos" ) );
				state.create_table();
				{
					auto index = 1;
					for ( auto cur : shot.penetration_points )
					{
						state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
						state.set_i( index++ );
					}
				}
				state.set_field( XOR_STR( "client_impacts" ) );
				state.create_table();
				{
					auto index = 1;
					for ( auto cur : shot.hitposes )
					{
						state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
						state.set_i( index++ );
					}
				}
				state.set_field( XOR_STR( "server_impacts" ) );
				return 1;
			} );

			if ( shot.hurt )
			{
				const auto player = globals::get_player( shot.hitinfo.victim );
				if ( player )
				{
					add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, shot.hitpos ) );

					if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
						add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
					continue;
				}
			}

			if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
				add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
			continue;
		}

		auto hitpos = get_closest_hitpos( shot, shot.hitgroup != -1 ? shot.hitpos : shot.record.m_origin );

		auto player = globals::get_player( shot.enemy_index );
		if ( vars::visuals.chams.enemy.shot_record.type->get<int>() && player )
			chams::add_ghost( player, &shot.record );

		if ( !player )
		{
			shot.hitpos = hitpos;
			if ( shot.hurt )
			{
				add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos ) );

				if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() && !beams::beam_exists( local_player, interfaces::globals()->curtime ) )
					add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );
			}
			else if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
				add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );

			continue;
		}

		if ( !local_player || !local_player->get_alive() || !local_weapon )
			continue;

		shot.hitpos = shot.hitposes[ shot.hitposes.size() - 1 ] + direction * 1000.f;

		auto& log = player_log::get_log( shot.enemy_index );
		auto data = penetration::pen_data( &shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data );

		if ( shot.record.m_shot_info.extrapolated && !log.record.empty() && !log.record.back().m_dormant )
		{
			data = penetration::pen_data( &log.record.back(), shot.record.m_shot_dir, false, nullptr, &shot.weapon_data );
		}

		aimbot::aimpoint_t aimpoint;
		aimpoint.hitbox = -1;
		aimpoint.point = end;

		auto damage = 0;
		auto new_data = data;
		if ( can_hit( local_player, new_data, shot.shotpos, &aimpoint, damage, true ) )
		{
			hitpos = get_closest_hitpos( shot, aimpoint.point );
			shot.hitpos = hitpos;
			shot.hit = true;
			shot.hit_originally = true;
		}

		const auto deltavec = Vector( shot.original_shotpos.x - shot.shotpos.x, shot.original_shotpos.y - shot.shotpos.y, 0 );
		const auto corrected_pos = fabsf( deltavec.x ) >= 0.001f || fabsf( deltavec.y ) >= 0.001f;

		if ( corrected_pos )
		{
			auto damage2 = 0;
			shot.hit_originally = can_hit( local_player, data, shot.original_shotpos, &aimpoint, damage2, true );
		}

		if ( shot.record.m_shot_info.extrapolated )
		{
			auto damage2 = 0;
			shot.hit_extrapolation = can_hit( local_player, penetration::pen_data( &shot.record, shot.record.m_shot_dir, false, nullptr, &shot.weapon_data ), shot.shotpos, &aimpoint, damage2, true );
		}

		if ( vars::visuals.beams.local.enabled->get<bool>() && vars::visuals.beams.enabled->get<bool>() )
			add_local_beam( beams::impact_info_t( interfaces::globals()->curtime, shot.shotpos, end, interfaces::engine()->GetLocalPlayer(), Color( vars::visuals.beams.local.color->get<D3DCOLOR>() ) ) );

		if ( shot.hurt )
			add_hit( hitmarker::hitmarker_t( interfaces::globals()->realtime, shot.hitinfo.victim, shot.hitinfo.damage, shot.hitinfo.hitgroup, hitpos ) );

		if ( shot.hitgroup == -1 )
			continue;

		Vector zerovec = {};

		lua::api.callback( FNV1A( "on_shot_registered" ), [&] ( lua::state& state )
		{
			state.create_table();
			state.set_field( XOR_STR( "manual" ), shot.hitgroup == -1 );
			state.set_field( XOR_STR( "secure" ), shot.safety >= penetration::safety_no_roll );
			state.set_field( XOR_STR( "very_secure" ), shot.safety >= penetration::safety_full );
			state.set_field( XOR_STR( "result" ), shot.hurt ? XOR_STR( "hit" ) : shot.hit ? XOR_STR( "resolve" ) : shot.hit_extrapolation ? ( !ConVar::cl_lagcompensation || !ConVar::cl_predict ) ? XOR_STR( "anti-exploit" ) : XOR_STR( "extrapolation" ) : shot.hit_originally ? XOR_STR( "server correction" ) : XOR_STR( "spread" ) );
			state.set_field( XOR_STR( "target" ), shot.enemy_index );
			state.set_field( XOR_STR( "tick" ), shot.tick );
			state.set_field( XOR_STR( "backtrack" ), shot.record.m_shot_info.backtrack_ticks );
			state.set_field( XOR_STR( "hitchance" ), shot.record.m_shot_info.hitchance );
			state.set_field( XOR_STR( "client_hitgroup" ), shot.hitgroup );
			state.set_field( XOR_STR( "client_damage" ), shot.damage );
			state.set_field( XOR_STR( "server_hitgroup" ), shot.hitinfo.hitgroup );
			state.set_field( XOR_STR( "server_damage" ), shot.hitinfo.damage );
			state.create_user_object<decltype( shot.shotpos )>( XOR_STR( "vec3" ), &shot.shotpos );
			state.set_field( XOR_STR( "shotpos" ) );
			state.create_user_object<decltype( end )>( XOR_STR( "vec3" ), &end );
			state.set_field( XOR_STR( "client_hitpos" ) );
			state.create_user_object<decltype( shot.hitpos )>( XOR_STR( "vec3" ), shot.hurt ? &shot.hitpos : &zerovec );
			state.set_field( XOR_STR( "server_hitpos" ) );
			state.create_table();
			{
				auto index = 1;
				for ( auto cur : shot.penetration_points )
				{
					state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
					state.set_i( index++ );
				}
			}
			state.set_field( XOR_STR( "client_impacts" ) );
			state.create_table();
			{
				auto index = 1;
				for ( auto cur : shot.hitposes )
				{
					state.create_user_object<decltype( cur )>( XOR_STR( "vec3" ), &cur );
					state.set_i( index++ );
				}
			}
			state.set_field( XOR_STR( "server_impacts" ) );
			return 1;
		} );

		if ( player->get_player_info().fakeplayer )
		{
			calc_missed_shots( &shot );
			continue;
		}

		if ( vars::legit_enabled() )
			continue;

		get_brute_angle( &shot );
		calc_missed_shots( &shot );
	}

	current_shots.clear();
	unapproved_shots.clear();
	current_hitposes.clear();
}

void resolver::get_brute_angle( shot_t* shot )
{
	if ( !local_player || !local_player->get_alive() || !local_weapon || shot->record.m_dormant )
		return;

	const auto player = globals::get_player( shot->enemy_index );
	if ( !player || !player->get_alive() )
		return;

	const auto hdr = player->get_model_ptr();
	if ( !hdr )
		return;

	const auto studio_hdr = hdr->m_pStudioHdr;
	if ( !studio_hdr )
		return;

	const auto hitbox_set = studio_hdr->pHitboxSet( player->get_hitbox_set() );
	if ( !hitbox_set )
		return;

	const auto hitbox = hitbox_set->pHitbox( HITBOX_HEAD );
	if ( !hitbox )
		return;

	const auto log = &player_log::get_log( shot->enemy_index );

	const auto use_front = shot->record.m_shot_info.extrapolated && !log->record.empty() && !log->record.back().m_dormant;
	const auto target_record = use_front ? &log->record.back() : &shot->record;
	if ( use_front )
		target_record->setup_matrices();

	const auto state = shot->record.m_shot_dir;
	const auto current_mode = target_record->m_shot ? resolver_mode::resolver_shot : shot->record.m_resolver_mode;
	const auto current_side = shot->record.m_resolver_side;

	const auto cureye = shot->record.m_eye_angles;
	const auto legit = fabsf( cureye.x ) < 60.f && ( shot->record.m_lagamt < 4 || shot->record.m_lagamt > 18 ) && !shot->record.m_shot && log->m_unknown;

	Vector shot_dir = {};
	const auto shot_angle = math::calc_angle( shot->shotpos, shot->hitpos );
	math::angle_vectors( shot_angle, &shot_dir );

	const auto end = shot->hitpos + shot_dir * 15.f;

	if ( !shot->hurt )
	{
		enum_array<resolver_direction, bool, resolver_direction::resolver_direction_max> new_blacklist = {};

		for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
		{
			aimbot::aimpoint_t aimpoint{};
			aimpoint.hitbox = -2;
			aimpoint.point = end;

			auto damage = 0;
			can_hit( local_player, penetration::pen_data( target_record, i, false, nullptr, &shot->weapon_data ), shot->shotpos, &aimpoint, damage, true );

			if ( damage > 1.f )
				new_blacklist[ i ] = log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist[ i ] = true;
		}

		if ( shot->hit )
		{
			auto furthest_angle = -FLT_MAX;
			auto furthest_dir = resolver_direction::resolver_invalid;

			const auto target_pos = Vector( target_record->m_state[ state ].m_matrix[ hitbox->bone ][ 0 ][ 3 ], target_record->m_state[ state ].m_matrix[ hitbox->bone ][ 1 ][ 3 ], target_record->m_state[ state ].m_matrix[ hitbox->bone ][ 2 ][ 3 ] );
			const auto target_state_ang = math::calc_angle( target_record->m_origin, target_pos );

			for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
			{
				if ( log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist[ i ] )
					continue;

				const auto pos = Vector( target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 0 ][ 3 ], target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 1 ][ 3 ], target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 2 ][ 3 ] );
				const auto angle = math::calc_angle( target_record->m_origin, pos );
				const auto diff = fabsf( math::normalize_float( angle.y - target_state_ang.y ) );
				if ( diff > furthest_angle )
				{
					furthest_dir = i;
					furthest_angle = diff;
				}
			}

			if ( furthest_dir != resolver_direction::resolver_invalid )
			{
				log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir = furthest_dir;
			}
			else
			{
				log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir = resolver_direction::resolver_networked;
				if ( log->nospread.m_can_fake && log->nospread.m_pitch_cycle % 2 == shot->record.m_pitch_cycle % 2 )
					log->nospread.m_pitch_cycle++;

				if ( globals::nospread )
					new_blacklist = {};
				else if ( new_blacklist[ resolver_direction::resolver_networked ] )
				{
					furthest_angle = -FLT_MAX;
					for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
					{
						if ( new_blacklist[ i ] )
							continue;

						const auto pos = Vector( target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 0 ][ 3 ], target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 1 ][ 3 ], target_record->m_state[ i ].m_matrix[ hitbox->bone ][ 2 ][ 3 ] );
						const auto angle = math::calc_angle( target_record->m_origin, pos );
						const auto diff = fabsf( math::normalize_float( angle.y - target_state_ang.y ) );
						if ( diff > furthest_angle )
						{
							log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir = i;
							furthest_angle = diff;
						}
					}
				}

				log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist = new_blacklist;
			}
		}
	}
	else if ( !legit )
	{
		enum_array<resolver_direction, bool, resolver_direction::resolver_direction_max> new_blacklist = {};
		enum_array<resolver_direction, float, resolver_direction::resolver_direction_max> hit_dist{};
		hit_dist.fill( FLT_MAX );

		for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
		{
			aimbot::aimpoint_t aimpoint{};
			aimpoint.hitbox = -2;
			aimpoint.point = end;

			auto damage = 0;
			can_hit( local_player, penetration::pen_data( target_record, i, false, nullptr, &shot->weapon_data ), shot->shotpos, &aimpoint, damage, true );

			if ( damage < 1.f || aimpoint.hitgroup != shot->hitinfo.hitgroup )
				new_blacklist[ i ] = log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist[ i ] = true;
			else
			{
				const auto hitpos = get_closest_hitpos( *shot, aimpoint.point );
				hit_dist[ i ] = aimpoint.point.Dist( hitpos );
			}
		}

		auto blacklist_full = true;
		auto blacklist_invalid = false;

		const auto prev_state = shot->record.m_shot_dir;

		auto closest = FLT_MAX;
		for ( auto i = resolver_direction::resolver_networked; i < resolver_direction::resolver_direction_max; i++ )
		{
			if ( !log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist[ i ] )
				blacklist_full = false;

			auto& cur = hit_dist[ i ];

			if ( log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist[ i ] && cur != FLT_MAX )
				blacklist_invalid = true;

			if ( cur < closest && hit_dist[ prev_state ] > 0.1f )
			{
				log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir = i;
				closest = cur;
			}
		}

		if ( blacklist_full || blacklist_invalid )
		{
			log->m_mode[ current_mode ].m_side[ current_side ].m_blacklist = new_blacklist;
		}
	}

	if ( log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir != state || shot->hurt )
	{
		if ( current_mode == resolver_mode::resolver_shot )
			log->m_unknown_shot = false;
		else
			log->m_unknown = false;
	}

	if ( log->m_unknown && player->get_alive() && log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir != state && current_mode != resolver_mode::resolver_shot && state != resolver_direction::resolver_networked )
	{
		const auto current_state = log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir;
		const auto opposite = current_state == resolver_direction::resolver_min ? resolver_direction::resolver_max
			: current_state == resolver_direction::resolver_max ? resolver_direction::resolver_min
			: resolver_direction::resolver_zero;

		log->m_mode[ static_cast< resolver_mode >( ( static_cast< int >( current_mode ) + 1 ) % 2 ) ].m_side[ current_side ].m_current_dir = opposite;
	}
}

void resolver::calc_missed_shots( shot_t* shot )
{
	const auto log = &player_log::get_log( shot->enemy_index );
	if ( shot->record.m_dormant )
	{
		if ( shot->hurt )
		{
			const auto sound_player = sound_esp::get_sound_player( shot->enemy_index );
			sound_player->last_update_tick = interfaces::client_state()->get_last_server_tick();
			sound_player->updated = true;
			log->m_dormant_misses = 0;
		}
		else
		{
			log->m_dormant_misses++;
			interfaces::cvar()->ConsoleColorPrintf( Color( 235, 5, 90, 255 ), xorstr_( "[fatality] " ) );
			util::print_dev_console( true, Color( 255, 255, 255, 255 ), xorstr_( "miss due to dormant aimbot\n" ) );
		}
		return;
	}

	if ( shot->hurt && globals::nospread && shot->hitinfo.hitgroup == HITGROUP_HEAD && !shot->record.m_shot )
		log->nospread.m_pitch_cycle = 0;

	const auto player = globals::get_player( shot->enemy_index );

	if ( shot->hurt )
		return;

	if ( shot->hit && player && player->get_alive() )
	{
		if ( shot->record.m_unknown )
			log->m_unknown_misses++;

		log->m_shots++;
		
		const auto current_mode = shot->record.m_resolver_mode;
		const auto current_side = shot->record.m_resolver_side;
		const auto desync_side = log->m_desync_side;
		
		if ( desync_side != 0 )
		{
			// Pass the just-missed direction as skip_dir so the adaptive
			// brute immediately moves to the next candidate instead of
			// re-trying the same angle. Use the cached aa_state from the
			// last resolve; if it wasn't set (e.g. dormant path), default
			// to normal.
			const auto aa_state = static_cast< enemy_aa_state_t >( log->m_resolver_state );
			log->m_mode[ current_mode ].m_side[ current_side ].m_current_dir = get_brute_direction( *log, desync_side, aa_state, shot->record.m_shot_dir );
		}
	}

	if ( shot->hit )
		return;

	log->m_shots_spread++;

	_( fatality, "FATALITY " );
	_( missed, "miss due to spread\n" );
	_( missed2, "miss due to server correction\n" );
	_( missed3, "miss due to extrapolation\n" );
	_( missed3_2, "miss due to anti-exploit\n" );

	interfaces::cvar()->ConsoleColorPrintf( Color( 235, 5, 90, 255 ), fatality.c_str() );

	if ( shot->hit_extrapolation )
		util::print_dev_console( true, Color( 255, 255, 255, 255 ), ( !ConVar::cl_lagcompensation || !ConVar::cl_predict ) ? missed3_2.c_str() : missed3.c_str() );
	else if ( shot->hit_originally )
		util::print_dev_console( true, Color( 255, 255, 255, 255 ), missed2.c_str() );
	else
		util::print_dev_console( true, Color( 255, 255, 255, 255 ), missed.c_str() );
}

void resolver::set_local_info()
{
	last_origin_diff = local_player->get_origin() - last_origin;
	last_eye = local_player->get_eye_pos();
	last_origin = local_player->get_origin();
	current_eye = local_player->get_eye_pos();
}
