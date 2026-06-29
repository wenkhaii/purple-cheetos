#pragma once

namespace lagcomp
{
	bool run_extrapolation( const C_CSPlayer* player, const bool simple = false );
	int fix_tickcount( const float& simtime );
	bool valid_simtime( const float& simtime, const float time = ticks_to_time( local_player->get_tickbase() - 1 ) );
	bool is_breaking_lagcomp( const C_CSPlayer* player );
	bool is_breaking_lagcomp( const C_CSPlayer* player, const lag_record_t* current );
	int get_real_lag( const C_CSPlayer* player, const lag_record_t* current );
	float get_lerp_time();
	void extrapolate( C_CSPlayer* player, Vector& origin, Vector& velocity, Vector base_velocity, int& flags, bool wasonground );

	// Extended backtrack methods (Neverlose-inspired)
	bool is_record_in_window( const lag_record_t* record, float sim_time, float window );
	void build_backtrack_records( int player_index, float window, std::vector<lag_record_t*>& out );
	void filter_extended_backtrack( const std::vector<lag_record_t*>& source, std::vector<lag_record_t*>& out );
};
