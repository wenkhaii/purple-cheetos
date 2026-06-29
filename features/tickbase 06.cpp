#include "../include_cheat.h"

void tickbase::reset()
{
	force_choke = force_drop = skip_next_adjust = fast_fire = hide_shot = keep_config_changed = false;
	clock_drift = server_limit = to_recharge = to_shift = to_adjust = 0;
}

bool tickbase::holds_tick_base_weapon()
{
	const auto wpn = local_weapon;
	if (!wpn)
		return false;

	const auto info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());

	if (!info)
		return false;

	return wpn->get_weapon_id() != WEAPON_TASER
		&& wpn->get_weapon_id() != WEAPON_FISTS
		&& wpn->get_weapon_id() != WEAPON_C4
		&& !wpn->is_grenade()
		&& wpn->GetClientClass()->m_ClassID != ClassId::CSnowball
		&& wpn->get_weapon_type() != WEAPONTYPE_UNKNOWN;
}

void tickbase::adjust_limit_dynamic(const bool finalize)
{
	const auto changed = apply_static_configuration();
	const auto ready = !to_recharge && !to_shift && !post_shift && !force_choke;

	if (changed)
		keep_config_changed = force_unchoke = true;

	const auto wpn = local_weapon;
	if (!wpn || !ready)
		return;

	const auto wpn_info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());
	if (!wpn_info)
		return;

	const auto cycle_diff = wpn->get_next_primary_attack() - interfaces::globals()->curtime;

	if (fast_fire && (wpn->is_shootable() || wpn->is_knife()) && (wpn_info->cycle_time < 0.55f && cycle_diff > -0.2f || cycle_diff > 0.70f) && (wpn->is_knife() || !wpn->in_reload()))
		return;

	auto dont_recharge = wpn->is_grenade() && (wpn->get_pin_pulled() || wpn->get_throw_time() != 0.f);

	if (dont_recharge)
		keep_config_changed = false;

	const auto diff_wpn = wpn->get_next_primary_attack() - interfaces::globals()->curtime;
	const float curtime = interfaces::globals()->curtime;
	const auto diff_player = local_player->get_next_attack() - interfaces::globals()->curtime;

	if (!dont_recharge && !changed && (fast_fire || hide_shot) && (wpn->is_shootable() || wpn->is_knife()) &&
		((wpn_info->cycle_time < .55f && diff_wpn > -.2f) || diff_wpn > .7f) && (wpn->is_knife() || !wpn->in_reload()))
		dont_recharge = true;

	if (!dont_recharge && diff_player > .7f)
		dont_recharge = true;

	if (keep_config_changed)
		dont_recharge = false;

	if (dont_recharge)
		to_recharge = 0;

	const auto diff = determine_optimal_limit() - compute_current_limit();
	const bool standing = local_player && local_player->get_velocity().Length() < 1.1f && globals::current_cmd && globals::current_cmd->forwardmove == 0.f && globals::current_cmd->sidemove == 0.f;

	if (diff >= -2 && diff <= 2) {
		if (!diff) keep_config_changed = false;
		return;
	}

	if (diff > 0 && (vars::aim.silent->get<bool>() || aimbot::last_target == -1) && !dont_recharge && !finalize && (diff > 2 || standing))
	{
		to_recharge = diff;
		prediction::take_shot(false);
		globals::current_cmd->buttons &= ~IN_ATTACK2;
	}
	else if (diff < 0 && !finalize)
	{
		to_shift = -diff;
	}

	if (!diff)
		keep_config_changed = false;

}

void tickbase::on_recharge(const CUserCmd* cmd)
{
	auto& info = prediction::get_pred_info(cmd->command_number);
	if (info.sequence != cmd->command_number)
	{
		info.reset();
		info.sequence = cmd->command_number;
	}

	info.tickbase.choked_commands += 1;
}

void tickbase::on_finish_command(bool sendpacket, const CUserCmd* cmd)
{
	auto& info = prediction::get_pred_info(cmd->command_number);
	if (info.sequence != cmd->command_number)
		return;

	if (to_shift > 0)
		info.tickbase.sent_commands += 1;

	if (sendpacket)
		fill_fake_commands();
}

void tickbase::on_send_command(const CUserCmd* cmd)
{
	to_adjust = 0;

	auto& info = prediction::get_pred_info(cmd->command_number);
	if (info.sequence != cmd->command_number)
		return;

	if (antiaim::started_peek_fakelag() && !to_shift && !to_recharge) // in onpeek and unduck
		skip_next_adjust = true;

	if (skip_next_adjust)
		interfaces::prediction()->get_predicted_commands() = clamp(interfaces::client_state()->lastoutgoingcommand - interfaces::client_state()->last_command_ack, 0, interfaces::prediction()->get_predicted_commands());
	else
		to_adjust = sv_maxusrcmdprocessticks - (interfaces::client_state()->chokedcommands + 1);

	apply_static_configuration();

	info.tickbase.skip_fake_commands = skip_next_adjust;

	compute_current_limit(cmd->command_number);
}

int tickbase::compute_current_limit(int command_number)
{
	if (!command_number)
		return 0;

	const auto& last_ack_info = prediction::get_pred_info(interfaces::client_state()->last_command_ack);

	const auto max_process_ticks = sv_maxusrcmdprocessticks;

	auto limit = last_ack_info.sequence == interfaces::client_state()->last_command_ack ? last_ack_info.tickbase.limit : 16;
	for (auto i = interfaces::client_state()->last_command_ack + 1; i <= command_number; i++)
	{
		auto& info = prediction::get_pred_info(i);
		if (info.sequence != interfaces::input()->m_pCommands[i % 150].command_number)
			continue;

		info.tickbase.limit = clamp(limit + info.tickbase.choked_commands, 0, max_process_ticks);
		info.tickbase.limit = std::max(info.tickbase.limit - info.tickbase.sent_commands, 0);
		limit = info.tickbase.limit;
	}

	return limit;
}

void tickbase::on_runcmd(const CUserCmd* cmd, int& tickbase)
{
	const auto& current_info = prediction::get_pred_info(cmd->command_number);

	if (cmd->command_number != current_info.sequence)
		return;

	auto to_adjust = 0;
	std::optional<bool> prev_skip_fake_commands;
	for (auto i = interfaces::client_state()->last_command_ack; i <= cmd->command_number; i++)
	{
		const auto& info = prediction::get_pred_info(i);
		if (info.sequence != interfaces::input()->m_pCommands[i % 150].command_number)
			continue;

		if (info.tickbase.choked_commands > 0)
		{
			prev_skip_fake_commands = false;
			continue;
		}

		if (!prev_skip_fake_commands.has_value())
			prev_skip_fake_commands = info.tickbase.skip_fake_commands;

		if (prev_skip_fake_commands != info.tickbase.skip_fake_commands)
			to_adjust = info.tickbase.skip_fake_commands ? info.tickbase.limit : -info.tickbase.limit;
		else
			to_adjust = 0;

		prev_skip_fake_commands = info.tickbase.skip_fake_commands;
	}

	/*if ( to_adjust != 0 )
		util::print_dev_console( true, Color::White(), "tickbase: %d adjusted %d\n", tickbase, to_adjust );*/

	tickbase += to_adjust;
}

void tickbase::attempt_shift_back(bool& send_packet)
{
	const auto weapon = local_weapon;
	const auto is_revolver = weapon->get_weapon_id() == WEAPON_REVOLVER;

	const auto dont = (fast_fire || hide_shot) && is_revolver || globals::shot_command <= interfaces::client_state()->lastoutgoingcommand || to_shift > 0;

	if (compute_current_limit() > 3 && local_player->get_tickbase() > animations::lag.first && !dont)
	{
		const auto predicted_time = interfaces::globals()->curtime + ticks_to_time(compute_current_limit());
		const auto release_tick = time_to_ticks(weapon->get_next_secondary_attack() - predicted_time);

		skip_next_adjust = !is_revolver || release_tick > 1 && release_tick < 10 - interfaces::client_state()->chokedcommands;
		if (skip_next_adjust)
			send_packet = true;

		if (!resolver::shots.empty())
			resolver::shots.pop_back();

		prediction::take_shot(false);
		if (!is_revolver)
			prediction::take_secondary_shot(false);
		globals::shot_command = 0;

		misc::retract_peek = false;

		return;
	}

	if (fast_fire)
	{
		to_shift = determine_optimal_shift();
		if (compute_current_limit() - to_shift < 3)
			to_shift = compute_current_limit();

		send_packet = true;
	}
}

void tickbase::revert_shift_back()
{
	to_shift = 0;
}

void tickbase::fill_fake_commands()
{
	skip_next_adjust = false;

	for (auto i = 0; i < to_adjust; i++)
	{
		interfaces::client_state()->chokedcommands++;
		const auto sequence = interfaces::client_state()->lastoutgoingcommand + interfaces::client_state()->chokedcommands + 1;
		const auto cmd = &interfaces::input()->m_pCommands[sequence % 150];
		*cmd = *globals::current_cmd;
		cmd->command_number = sequence;
		cmd->tick_count = globals::current_cmd->tick_count + 200 + i;
		cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
		misc::write_tick(cmd->command_number);
	}
}

//void tickbase::apply_static_configuration()
bool tickbase::apply_static_configuration()
{
	const auto previous = fast_fire || hide_shot;
	//const auto wpn = local_weapon;
	if (vars::aim.fake_duck->get<bool>())
	{
		fast_fire = hide_shot = false;
		//return;
	}
	else //if (wpn)
	{
		fast_fire = vars::aim.doubletap->get<bool>();
		hide_shot = !fast_fire && vars::aim.silent->get<bool>();
	}

	return previous != (fast_fire || hide_shot);

}

int tickbase::determine_optimal_shift()
{
	const auto wpn = local_weapon;
	if (!wpn)
		return 0;

	const auto info = interfaces::weapon_system()->GetWpnData(wpn->get_weapon_id());
	if (!info)
		return 0;

	constexpr auto min_shift_amt = 4;
	const auto max_shift_amt = compute_current_limit();

	return clamp(wpn->is_secondary_attack_weapon() || wpn->get_weapon_id() == WEAPON_REVOLVER || vars::misc.peek_assist->get<bool>() ? max_shift_amt : time_to_ticks(info->cycle_time) - 1, min_shift_amt, max_shift_amt);
}

int tickbase::determine_optimal_limit()
{
	if (fast_fire || hide_shot)
		return max_new_cmds;

	return 0;
}

float tickbase::get_adjusted_time()
{
	return ticks_to_time(local_player->get_tickbase() - 1);
}

bool tickbase::is_ready()
{
	return !to_recharge && !to_shift;
}