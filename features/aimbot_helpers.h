#pragma once

namespace aimbot_helpers
{
	struct choked_shot_t
	{
		choked_shot_t( QAngle angles, resolver::shot_t shot )
		{
			this->manual = true;
			this->shot = shot;
			this->angles = angles;
			this->tickcount = -1;
		}
		choked_shot_t( QAngle angles, int tickcount, resolver::shot_t shot )
		{
			this->manual = false;
			this->shot = shot;
			this->angles = angles;
			this->tickcount = tickcount;
		}
		bool manual = false;
		QAngle angles{};
		int tickcount = -1;
		resolver::shot_t shot{};
	};

	void no_recoil( CUserCmd* cmd = globals::current_cmd );
	void no_visual_recoil( CViewSetup& v_view );
	bool autorevolver( C_WeaponCSBaseGun* weapon );
	void slide_stop( CUserCmd* cmd = globals::current_cmd );
	void stop_to_speed( float speed, CUserCmd* cmd = globals::current_cmd );
	void stop_to_speed( float speed, CMoveData* data, C_CSPlayer* player = local_player );
	void draw_debug_hitboxes( C_CSPlayer* player, matrix3x4_t* matrices, int hitbox, float duration, Color color = Color::Green() );
	void remove_spread( CUserCmd* cmd = globals::current_cmd );
	void manage_lagcomp();
	float get_lowest_inaccuracy( C_WeaponCSBaseGun* weapon );
	bool highest_accuracy( C_WeaponCSBaseGun* weapon, bool check_landing = false );
	void build_seed_table();
	std::tuple<float,float> calc_hc( std::vector<std::shared_ptr<intersection>>& intersections, QAngle vangles, lag_record_t& record );
	float calc_penhc( aimbot::final_target_t* target, const bool skip_full = false, const bool maximum = false );
	bool is_better_target( const aimbot::final_target_t& candidate, const aimbot::final_target_t& current, int last_target_index = -1 );
	void check_corner_hitpoint( aimbot::final_target_t& target );
	bool get_current_autostop();
	bool should_autostop( const aimbot::final_target_t& final_target );

	// Returns true when the weapon is in the cocking/cycling window between shots
	// (cannot fire because next_primary_attack is in the future, but NOT because of
	//  reload, inaccuracy, or dt recharge). This is the window where the player
	// should be allowed to move freely instead of being forced to slow-walk.
	bool is_between_shots();
	// Returns true while the weapon is cocking/cycling the next bullet and we are
	// still far enough from the next fire time that releasing the stop is safe.
	// The last `prepare_ticks` ticks before the next shot we re-acquire the stop so
	// the player is settled and accurate for the next shot.
	bool between_shots_allow_movement( int prepare_ticks = 4 );
	float get_final_hitchance( C_WeaponCSBaseGun* weapon, const int index );
	bool check_hitchance( aimbot::final_target_t& final_target );
	bool check_max_hitchance( aimbot::final_target_t& final_target );

	inline int tick_cocked = {};
	inline int tick_strip = {};
	inline std::vector<std::tuple<float, float, float>> precomputed_seeds = {};
};