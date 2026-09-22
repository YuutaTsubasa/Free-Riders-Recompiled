param([int]$Jobs = 4, [switch]$Diagnostic, [string]$DiagnosticDirectory = 'out/recomp/diagnostic')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ desktop tools are required.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$compiler = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "Install LLVM with clang-cl at $compiler" }
# Import the compiler/SDK environment into this process only.
$environmentLines = & $env:COMSPEC /d /s /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:PATH = (Split-Path -Parent $compiler) + ';' + $env:PATH
$source = $repoRoot
$build = Join-Path $repoRoot 'out\build\host'
$diagnosticOption = if ($Diagnostic) { 'ON' } else { 'OFF' }
$diagnosticPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $DiagnosticDirectory))
& cmake -S $source -B $build -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_CXX_COMPILER=$compiler" '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' "-DSFR_BUILD_DIAGNOSTIC=$diagnosticOption" "-DSFR_DIAGNOSTIC_DIR=$diagnosticPath"
if ($LASTEXITCODE -ne 0) { throw 'Xenon tools configure failed.' }
$targets = @('XenonAnalyse', 'XenonRecomp', 'sfr_image_dump', 'sfr_memory_test', 'sfr_xex_module_test', 'sfr_virtual_memory_test', 'sfr_critical_section_test', 'sfr_hardware_info_test', 'sfr_thread_local_storage_test', 'sfr_system_time_test', 'sfr_guest_clock_test', 'sfr_timestamp_bundle_test')
$targets += 'sfr_vector_memory_test'
$targets += 'sfr_debug_monitor_test'
$targets += 'sfr_nui_device_status_test'
$targets += 'sfr_nui_skeleton_test'
$targets += 'sfr_nui_speech_test'
$targets += 'sfr_nui_race_test'
$targets += 'sfr_shader_inputs_test'
$targets += 'sfr_loop_constants_test'
$targets += 'sfr_vertex_palette_test'
$targets += 'sfr_unselected_users_test'
$targets += 'sfr_content_files_test'
$targets += 'sfr_touch_controls_test'
$targets += 'sfr_multibyte_unicode_test'
$targets += 'sfr_render_state_entries_test'
$targets += 'sfr_video_globals_test'
$targets += 'sfr_counted_branch_test'
$targets += 'sfr_image_protection_test'
$targets += 'sfr_optional_import_policy_test'
$targets += 'sfr_native_modules_test'
$targets += 'sfr_system_config_test'
$targets += 'sfr_native_language_test'
$targets += 'sfr_native_country_test'
$targets += 'sfr_system_country_test'
$targets += 'sfr_guest_critical_sections_test'
$targets += 'sfr_game_region_test'
$targets += 'sfr_integer_arithmetic_test'
$targets += 'sfr_store_halfword_update_test'
$targets += 'sfr_store_float_single_update_test'
$targets += 'sfr_load_halfword_update_test'
$targets += 'sfr_memory_update_forms_test'
$targets += 'sfr_vector_integer_test'
$targets += 'sfr_native_input_test'
$targets += 'sfr_native_audio_test'
$targets += 'sfr_launcher_settings_test'
$targets += 'sfr_installer_test'
$targets += 'sfr_portable_waitables_test'
$targets += 'sfr_vulkan_shader_source_test'
$targets += 'sfr_launcher_art_test'
$targets += 'sfr_launcher'
$targets += 'sfr_texture_fetch_test'
$targets += 'sfr_memory_statistics_test'
$targets += 'sfr_video_mode_test'
$targets += 'sfr_physical_memory_test'
$targets += 'sfr_resource_coherency_test'
$targets += 'sfr_ansi_string_test'
$targets += 'sfr_native_sync_objects_test'
$targets += 'sfr_native_thread_test'
$targets += 'sfr_guest_execution_test'
$targets += 'sfr_guest_threads_test'
$targets += 'sfr_guest_sync_objects_test'
$targets += 'sfr_native_named_sync_test'
$targets += 'sfr_guest_named_sync_test'
$targets += 'sfr_guest_wait_test'
$targets += 'sfr_asset_files_test'
$targets += 'sfr_async_asset_files_test'
$targets += 'sfr_pending_guest_write_test'
$targets += 'sfr_native_async_read_test'
$targets += 'sfr_async_completion_primitives_test'
$targets += 'sfr_guest_async_files_test'
$targets += 'sfr_guest_files_test'
$targets += 'sfr_guest_file_read_test'
$targets += 'sfr_native_graphics_test'
$targets += 'sfr_native_shaders_test'
$targets += 'sfr_native_presentation_test'
$targets += 'sfr_native_raster_state_test'
$targets += 'sfr_native_blend_control_test'
$targets += 'sfr_guest_graphics_test'
$targets += 'sfr_native_formats_test'
$targets += 'sfr_guest_render_state_test'
$targets += 'sfr_guest_blend_request_test'
$targets += 'sfr_guest_sampler_filter_test'
$targets += 'sfr_guest_texture_binding_test'
$targets += 'sfr_native_notification_event_test'
$targets += 'sfr_native_notifications_test'
$targets += 'sfr_notification_placement_test'
if ($Diagnostic) { $targets += 'sfr_cpu_diagnostic'; $targets += 'sfr_native_winsock_test' }
& cmake --build $build --target $targets --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Xenon tools build failed.' }
