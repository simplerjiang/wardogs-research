#include <impl/include.hxx>
#include <channel/channel_protocol.hxx>

#include <cstring>
#include <fstream>
#include <sstream>

// hello from zxeno, all of the commented out stuff ignore unless you know what you are doing.

bool callback( ULONG64* param_one, ULONG64* param_two, ULONG64 allocation_pointer, ULONG64 allocation_size )
{
	UNREFERENCED_PARAMETER( param_two );
	UNREFERENCED_PARAMETER( allocation_size );
	/*auto* handshake = reinterpret_cast< channel::map_handshake* >(*param_one);
	if ( handshake && handshake->magic == channel::handshake_magic )
		handshake->driver_image_base = allocation_pointer;*/
	return true;
};

std::wstring get_exe_directory( void );

std::wstring get_channel_directory( void )
{
	wchar_t temp_path[ MAX_PATH ]{};
	const DWORD length = GetTempPathW( MAX_PATH, temp_path );
	if ( !length || length >= MAX_PATH )
		return get_exe_directory( );
	std::wstring directory( temp_path );
	if ( !directory.empty( ) && directory.back( ) != L'\\' && directory.back( ) != L'/' )
		directory += L'\\';
	directory += skCrypt( L"zxmapper" );
	std::filesystem::create_directories( directory );
	return directory;
};

bool write_channel_session( const channel::map_handshake& handshake )
{
	/*const auto session_path = get_channel_directory() + std::wstring(skCrypt(L"\\channel.session"));
	std::ofstream session_file( session_path, std::ios::binary | std::ios::trunc );
	if ( !session_file )
		return false;
	channel::session_file session{
		channel::session_magic,
		handshake.client_pid,
		0,
		handshake.shared_user_va,
		handshake.session_key,
		handshake.driver_image_base,
	};
	session_file.write( reinterpret_cast< const char* >( &session ), sizeof( session ) );
	return static_cast< bool >( session_file );*/
};

std::wstring get_exe_directory( void )
{
	wchar_t path[ MAX_PATH ];
	GetModuleFileNameW( NULL, path, MAX_PATH );
	return std::filesystem::path( path ).parent_path( );
};

int main( int argc, char* argv[] )
{
	// package fix: accept --auto in ANY position, so the launcher form
	// "kd2.exe driver.sys --auto" still runs unattended (upstream only
	// looked at argv[1], which parked the mapper on std::cin.get()).
	bool auto_mode = false;
	for ( int i = 1; i < argc; ++i )
		if ( argv[ i ] && std::strcmp( argv[ i ], skCrypt( "--auto" ) ) == 0 )
			auto_mode = true;
	if ( !NT_SUCCESS( intel_driver::load( ) ) )
	{
		util::log_error( skCrypt( "Vulnerable driver load failed" ) );
		if ( !auto_mode )
			std::cin.get( );
		return 1;
	};
	util::log( skCrypt( "Vulnerable driver loaded" ) );
	intel_driver::cache_trace_cleanup_context( );
	util::log( skCrypt( "Cleaning driver traces" ) );
	// [mrzv] CI hook DISABLED: patching ci.dll code (xor rax,rax;ret over a
	// CI function) is a direct kernel-code modification → 0x109 BSOD
	// (CRITICAL_STRUCTURE_CORRUPTION, ci.dll culprit) on Win11 26200.
	intel_driver::perform_load_trace_cleanup( false );
	std::wstring driver_path;
	std::wstring local_path = get_exe_directory( ) + std::wstring( skCrypt( L"\\kernel-mode.sys" ) );
	if ( std::filesystem::exists( local_path ) )
		driver_path = local_path;
	else
	{
		util::log_error( skCrypt( "kernel-mode.sys is missing" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	std::vector< uint8_t > raw_image;
	if ( !util::read_file_to_memory( driver_path, &raw_image ) )
	{
		util::log_error( skCrypt( "Failed to read driver image" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	mapper::allocation_mode mode = mapper::allocation_mode::allocate_pool;
	std::uint32_t client_pid = GetCurrentProcessId( );
	/*const auto target_path = get_channel_directory( ) + std::wstring( skCrypt( L"\\channel.target" ) );
	if ( std::filesystem::exists( target_path ) )
	{
		std::ifstream target_file( target_path );
		std::uint32_t file_pid = 0;
		target_file >> file_pid;
		if ( file_pid )
			client_pid = file_pid;
    };
	channel::map_handshake handshake{};
	handshake.magic = channel::handshake_magic;
	handshake.client_pid = client_pid;
	handshake.bootstrap_pid = GetCurrentProcessId( );*/
	util::log( skCrypt( "Mapping kernel driver" ) );
	NTSTATUS exit_code = 0;
	const ULONG64 mapped_base = mapper::map_driver(
		raw_image.data( ),
		0, // reinterpret_cast< ULONG64 >( &handshake )
		0,
		false,
		true,
		mode,
		false,
		callback,
		&exit_code );
	if ( !mapped_base )
	{
		util::log_error( skCrypt( "map_driver failed" ) );
		if ( !auto_mode )
			std::cin.get( );
		intel_driver::unload( );
		return 1;
	};
	{
		std::ostringstream stream;
		stream << skCrypt( "DriverEntry " ) << util::ntstatus_to_string( exit_code ) << skCrypt( " (0x" ) << std::hex << exit_code << std::dec << ')';
		util::log( stream.str( ) );
    };
	/*if (handshake.shared_user_va)
	{

		{
			std::ostringstream stream;
			stream << skCrypt( "Channel mapped | UVA 0x" ) << std::hex << std::uppercase << handshake.shared_user_va
				<< skCrypt( " | KVA 0x" ) << handshake.shared_kernel_va << skCrypt( " | Session 0x" ) << handshake.session_key << std::dec << std::nouppercase;
			util::log( stream.str( ) );
    };
		if ( write_channel_session( handshake ) )
			util::log( skCrypt( "channel.session written" ) );
		else
			util::log_error( skCrypt( "channel.session write failed" ) );
	}
	else
		util::log_error( skCrypt( "Channel not initialized" ) );*/
	util::log( skCrypt( "Cleaning driver traces" ) );
	intel_driver::cache_trace_cleanup_context( );
	intel_driver::perform_load_trace_cleanup( false );
	const NTSTATUS unload_status = intel_driver::unload( );
	if ( !NT_SUCCESS( unload_status ) )
	{
		std::ostringstream stream;
		stream << skCrypt( "Vulnerable driver unload failed (0x" ) << std::hex << unload_status << std::dec << ')';
		util::log_error( stream.str( ) );
	}
	else
		util::log( skCrypt( "Vulnerable driver unloaded" ) );
	if ( intel_driver::is_running( ) )
		util::log_error( skCrypt( "Vulnerable driver is still active, restart or unload manually" ) );
	if ( !auto_mode )
		std::cin.get( );
	return 0;
};