#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ShlObj.h>
#include <psapi.h>
#include "graphics-hook.h"
#include "../obfuscate.h"
#include "../funchook.h"

#define DEBUG_OUTPUT

#ifdef DEBUG_OUTPUT
#define DbgOut(x) OutputDebugStringA(x)
#else
#define DbgOut(x)
#endif

struct thread_data {
	CRITICAL_SECTION       mutexes[NUM_BUFFERS];
	CRITICAL_SECTION       data_mutex;
	void *volatile         cur_data;
	uint8_t                *shmem_textures[2];
	HANDLE                 copy_thread;
	HANDLE                 copy_event;
	HANDLE                 stop_event;
	volatile int           cur_tex;
	unsigned int           pitch;
	unsigned int           cy;
	volatile bool          locked_textures[NUM_BUFFERS];
};

ipc_pipe_client_t              pipe                            = {0};
HANDLE                         signal_restart                  = NULL;
HANDLE                         signal_stop                     = NULL;
HANDLE                         signal_ready                    = NULL;
HANDLE                         signal_exit                     = NULL;
HANDLE                         tex_mutexes[2]                  = {NULL, NULL};
static HANDLE                  filemap_hook_info               = NULL;

static HINSTANCE               dll_inst                        = NULL;
static volatile bool           stop_loop                       = false;
static HANDLE                  capture_thread                  = NULL;
char                           system_path[MAX_PATH]           = {0};
char                           process_name[MAX_PATH]          = {0};
char                           keepalive_name[64]              = {0};
HWND                           dummy_window                    = NULL;

static wchar_t                 system_path_w[MAX_PATH]         = {0};
static UINT                    system_path_w_len               = 0;

static unsigned int            shmem_id_counter                = 0;
static void                    *shmem_info                     = NULL;
static HANDLE                  shmem_file_handle               = 0;

static struct thread_data      thread_data                     = {0};

volatile bool                  active                          = false;
struct hook_info               *global_hook_info               = NULL;

HMODULE                        overlay_dll                     = NULL;
struct overlay_info            overlay_info                    = {0};

static bool                    log_buffer_enabled              = false;
static INIT_ONCE               log_buffer_cs_init              = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION        log_buffer_cs                   = {0};
static char                    *log_buffer                     = NULL;
static size_t                  log_buffer_size                 = 0;
static char                    *log_buffer_write               = NULL;
static char                    *log_buffer_write_reset         = NULL;
static size_t                  log_buffer_dropped_messages     = 0;

static HANDLE                  init_log_file                  = NULL;


static void open_init_log(void)
{
#ifdef HOOK_INIT_LOG_PATH
	wchar_t log_file_path[MAX_PATH];
	wchar_t *fpath;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &fpath)))
		return;

	int res = swprintf(log_file_path, MAX_PATH, L"%s/" TEXT(HOOK_INIT_LOG_PATH) L"/hook-init-log-%ld.log", fpath, GetCurrentProcessId());
	CoTaskMemFree(fpath);

	if (res < 1)
		return;

	log_file_path[MAX_PATH - 1] = 0;
	init_log_file = CreateFileW(log_file_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (init_log_file == INVALID_HANDLE_VALUE)
		init_log_file = NULL;
#endif
}

static void close_init_log(void)
{
	if (!init_log_file)
		return;

	CloseHandle(init_log_file);
	init_log_file = NULL;
}

static void init_log(const char *format, ...)
{
	if (!init_log_file)
		return;

	va_list args;
	char message[1024] = "";

	va_start(args, format);
	int num = _vsprintf_p(message, 1024, format, args);
	va_end(args);
	if (num > 0) {
		DWORD bytes_written;
		WriteFile(init_log_file, message, num, &bytes_written, NULL);
		WriteFile(init_log_file, "\n", 1, &bytes_written, NULL);
	}
}

static inline void wait_for_dll_main_finish(HANDLE thread_handle)
{
	if (thread_handle) {
		WaitForSingleObject(thread_handle, 100);
		CloseHandle(thread_handle);
	}
}

bool init_pipe(void)
{
	char new_name[64];
	sprintf(new_name, "%s%lu", PIPE_NAME, GetCurrentProcessId());

	if (!ipc_pipe_client_open(&pipe, new_name)) {
		DbgOut("Failed to open pipe\n");
		return false;
	}

	if (log_buffer_enabled && TryEnterCriticalSection(&log_buffer_cs)) {
		if (log_buffer_write_reset != log_buffer_write) {
			if (!ipc_pipe_client_write(&pipe, log_buffer, log_buffer_write - log_buffer + 1)) {
				LeaveCriticalSection(&log_buffer_cs);
				return false;
			}

			log_buffer_write = log_buffer_write_reset;
		}

		if (log_buffer_dropped_messages) {
			hlog("%d messages were dropped", log_buffer_dropped_messages);
			log_buffer_dropped_messages = 0;
		}

		LeaveCriticalSection(&log_buffer_cs);
	}

	return true;
}

static HANDLE init_event(const char *name, DWORD pid)
{
	HANDLE handle = get_event_plus_id(name, pid);
	if (!handle)
		init_log("Failed to get event '%s': %lu", name, GetLastError());
	return handle;
}

static HANDLE init_mutex(const char *name, DWORD pid)
{
	char new_name[64];
	HANDLE handle;

	sprintf(new_name, "%s%lu", name, pid);

	handle = OpenMutexA(MUTEX_ALL_ACCESS, false, new_name);
	if (!handle)
		init_log("Failed to open mutex '%s': %lu", name, GetLastError());
	return handle;
}

static inline bool init_signals(void)
{
	DWORD pid = GetCurrentProcessId();

	signal_restart = init_event(EVENT_CAPTURE_RESTART, pid);
	if (!signal_restart) {
		return false;
	}

	signal_stop = init_event(EVENT_CAPTURE_STOP, pid);
	if (!signal_stop) {
		return false;
	}

	signal_ready = init_event(EVENT_HOOK_READY, pid);
	if (!signal_ready) {
		return false;
	}

	signal_exit = init_event(EVENT_HOOK_EXIT, pid);
	if (!signal_exit) {
		return false;
	}

	return true;
}

static inline bool init_mutexes(void)
{
	DWORD pid = GetCurrentProcessId();

	tex_mutexes[0] = init_mutex(MUTEX_TEXTURE1, pid);
	if (!tex_mutexes[0]) {
		return false;
	}

	tex_mutexes[1] = init_mutex(MUTEX_TEXTURE2, pid);
	if (!tex_mutexes[1]) {
		return false;
	}

	return true;
}

static inline bool init_system_path(void)
{
	UINT ret = GetSystemDirectoryA(system_path, MAX_PATH);
	if (!ret) {
		init_log("Failed to get windows system path: %lu", GetLastError());
		return false;
	}

	system_path_w_len = GetSystemDirectoryW(system_path_w,
			sizeof(system_path_w) / sizeof(system_path_w[0]));
	if (!system_path_w_len ||
		system_path_w_len > sizeof(system_path_w) / sizeof(system_path_w[0])) {

		init_log("Failed to get unicode system path (%lu <-> %lu): %lu",
			system_path_w_len, sizeof(system_path_w) / sizeof(system_path_w[0]),
			GetLastError());
		system_path_w_len = 0;
	}

	return true;
}

static inline void log_current_process(void)
{
	DWORD len = GetModuleBaseNameA(GetCurrentProcess(), NULL, process_name,
			MAX_PATH);
	if (len > 0) {
		process_name[len] = 0;
		hlog("Hooked to process: %s", process_name);
	}

	hlog("(half life scientist) everything..  seems to be in order");
}

static inline bool init_hook_info(void)
{
	filemap_hook_info = get_hook_info(GetCurrentProcessId());
	if (!filemap_hook_info) {
		init_log("Failed to create hook info file mapping: %lu",
				GetLastError());
		return false;
	}

	global_hook_info = MapViewOfFile(filemap_hook_info, FILE_MAP_ALL_ACCESS,
			0, 0, sizeof(struct hook_info));
	if (!global_hook_info) {
		init_log("Failed to map the hook info file mapping: %lu",
				GetLastError());
		return false;
	}

	return true;
}

static void free_overlay(void);
void init_overlay_info(void)
{
	if (!global_hook_info->overlay_dll_path[0])
		return;

	overlay_dll = LoadLibraryA(global_hook_info->overlay_dll_path);
	if (!overlay_dll) {
		hlog("Failed to load overlay library '%s' (0x%x)",
				global_hook_info->overlay_dll_path,
				GetLastError());
		return;
	}

#define LOAD_SYM(x) \
	overlay_info.x = (overlay_ ## x ## _t) \
		GetProcAddress(overlay_dll, "overlay_" #x)
	LOAD_SYM(init);
	LOAD_SYM(free);

	if (overlay_info.init && !overlay_info.init(hlog,
		d3d9_create_shared_tex_, d3d9_luid)) {
		hlog("Overlay init returned false");
		free_overlay();
		return;
	}

	LOAD_SYM(reset);
	LOAD_SYM(compile_dxgi_shaders);
	//LOAD_SYM(draw_ddraw);
	LOAD_SYM(draw_d3d8);
	LOAD_SYM(draw_d3d9);
	LOAD_SYM(draw_d3d10);
	LOAD_SYM(draw_d3d11);
	LOAD_SYM(draw_gl);
#undef LOAD_SYM
}

#define DEF_FLAGS (WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)

static DWORD WINAPI dummy_window_thread(LPVOID *unused)
{
	static const wchar_t dummy_window_class[] = L"temp_d3d_window_4039785";
	WNDCLASSW wc;
	MSG msg;

	memset(&wc, 0, sizeof(wc));
	wc.style = CS_OWNDC;
	wc.hInstance = dll_inst;
	wc.lpfnWndProc = (WNDPROC)DefWindowProc;
	wc.lpszClassName = dummy_window_class;

	if (!RegisterClass(&wc)) {
		hlog("Failed to create temp D3D window class: %lu",
				GetLastError());
		return 0;
	}

	dummy_window = CreateWindowExW(0, dummy_window_class, L"Temp Window",
			DEF_FLAGS, 0, 0, 1, 1, NULL, NULL, dll_inst, NULL);
	if (!dummy_window) {
		hlog("Failed to create temp D3D window: %lu", GetLastError());
		return 0;
	}

	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	(void)unused;
	return 0;
}

static inline void init_dummy_window_thread(void)
{
	HANDLE thread = CreateThread(NULL, 0, dummy_window_thread, NULL, 0,
			NULL);
	if (!thread) {
		hlog("Failed to create temp D3D window thread: %lu",
				GetLastError());
		return;
	}

	CloseHandle(thread);
}

static inline bool init_hook(HANDLE thread_handle)
{
	wait_for_dll_main_finish(thread_handle);

	sprintf(keepalive_name, "%s%lu", EVENT_HOOK_KEEPALIVE,
			GetCurrentProcessId());

	if (!init_pipe()) {
		init_log("init_pipe failed");
		return false;
	}
	if (!init_signals()) {
		return false;
	}
	if (!init_mutexes()) {
		return false;
	}
	if (!init_system_path()) {
		return false;
	}
	if (!init_hook_info()) {
		return false;
	}

	init_overlay_info();

	init_dummy_window_thread();
	log_current_process();

	SetEvent(signal_restart);
	return true;
}

static inline void close_handle(HANDLE *handle)
{
	if (*handle) {
		CloseHandle(*handle);
		*handle = NULL;
	}
}

static void free_overlay(void)
{
	if (!overlay_dll)
		return;

	if (overlay_info.free)
		overlay_info.free();

	memset(&overlay_info, 0, sizeof(overlay_info));
	FreeLibrary(overlay_dll);
	overlay_dll = NULL;
}

static void free_hook(void)
{
	SetEvent(signal_exit);

	if (filemap_hook_info) {
		CloseHandle(filemap_hook_info);
		filemap_hook_info = NULL;
	}

	free_overlay();

	if (global_hook_info) {
		UnmapViewOfFile(global_hook_info);
		global_hook_info = NULL;
	}

	close_handle(&tex_mutexes[1]);
	close_handle(&tex_mutexes[0]);
	close_handle(&signal_exit);
	close_handle(&signal_ready);
	close_handle(&signal_stop);
	close_handle(&signal_restart);
	ipc_pipe_client_free(&pipe);
}

static inline bool d3d8_hookable(void)
{
	return !!global_hook_info->offsets.d3d8.present;
}

static inline bool ddraw_hookable(void)
{
	return !!global_hook_info->offsets.ddraw.surface_create &&
		!!global_hook_info->offsets.ddraw.surface_restore &&
		!!global_hook_info->offsets.ddraw.surface_release &&
		!!global_hook_info->offsets.ddraw.surface_unlock &&
		!!global_hook_info->offsets.ddraw.surface_blt &&
		!!global_hook_info->offsets.ddraw.surface_flip &&
		!!global_hook_info->offsets.ddraw.surface_set_palette &&
		!!global_hook_info->offsets.ddraw.palette_set_entries;
}

static inline bool d3d9_hookable(void)
{
	return !!global_hook_info->offsets.d3d9.present &&
		!!global_hook_info->offsets.d3d9.present_ex &&
		!!global_hook_info->offsets.d3d9.present_swap;
}

static inline bool dxgi_hookable(void)
{
	return !!global_hook_info->offsets.dxgi.present &&
		!!global_hook_info->offsets.dxgi.resize;
}

static HMODULE modules[1024];
static wchar_t module_path[1024];
static void list_process_modules(void)
{
	DWORD actual_size;

	SetLastError(0);
	if (!EnumProcessModulesEx(GetCurrentProcess(), modules, sizeof(modules), &actual_size, LIST_MODULES_ALL)) {
		hlog("list_process_modules: EnumProcessModulesEx failed (%#x)", GetLastError());
		return;
	}

	if (actual_size > sizeof(modules)) {
		hlog("list_process_modules: EnumProcessModulesEx required more space than provided (%u > %u)", actual_size, sizeof(modules));
		return;
	}

	for (size_t i = 0, count = actual_size / sizeof(modules[0]); i < count; i++) {
		UINT length = GetModuleFileNameEx(GetCurrentProcess(), modules[i], module_path, sizeof(module_path) / sizeof(module_path[0]));
		if (!length || length > sizeof(module_path) / sizeof(module_path[0])) {
			hlog("list_process_modules: GetModuleFileNameEx failed (%lu <-> %lu): %#x",
				length, length > sizeof(module_path) / sizeof(module_path[0]),
				GetLastError());
			continue;
		}

		if (system_path_w_len && system_path_w_len <= length &&
			CompareStringOrdinal(system_path_w, system_path_w_len,
				module_path, system_path_w_len, true) == CSTR_EQUAL)
			continue;

		hlog("list_process_modules: %ls", module_path);
	}
}

static inline bool attempt_hook(void)
{
	//static bool ddraw_hooked = false;
	static bool d3d8_hooked  = false;
	static bool d3d9_hooked  = false;
	static bool d3d9_broken  = false;
	static bool dxgi_hooked  = false;
	static bool dxgi_broken  = false;
	static bool gl_hooked    = false;
	static bool gl_broken    = false;
	static bool modules_logged = false;

	if (!d3d9_hooked && d3d9_hookable()) {
		d3d9_hooked = hook_d3d9();
	} else if (d3d9_hooked && !d3d9_broken) {
		d3d9_broken = !check_d3d9();
	}

	if (!dxgi_hooked && dxgi_hookable()) {
		dxgi_hooked = hook_dxgi();
	} else if (dxgi_hooked && !dxgi_broken) {
		dxgi_broken = !check_dxgi();
	}

	if (!gl_hooked) {
		gl_hooked = hook_gl();
	} else if (gl_hooked && !gl_broken) {
		gl_broken = !check_gl();
	/*} else {
		rehook_gl();*/
	}

	if (!d3d8_hooked && d3d8_hookable()) {
		d3d8_hooked = hook_d3d8();
	}

	/*if (!ddraw_hooked && ddraw_hookable()) {
		ddraw_hooked = hook_ddraw();
	}*/

	if (!modules_logged &&
		(d3d9_broken || dxgi_broken || gl_broken)) {
		hlog("attempt_hook: some hooks appear to be broken (d3d9: %d, dxgi: %d, gl: %d)",
			d3d9_broken, dxgi_broken, gl_broken);
		list_process_modules();
		modules_logged = true;
	}

	return d3d8_hooked || d3d9_hooked ||
		dxgi_hooked || gl_hooked;
}

static inline void capture_loop(void)
{
	while (!attempt_hook())
		Sleep(40);

	for (size_t n = 0; !stop_loop; n++) {
		/* this causes it to check every 4 seconds, but still with
		 * a small sleep interval in case the thread needs to stop */
		if (n % 100 == 0) attempt_hook();
		Sleep(40);
	}
}

static DWORD WINAPI main_capture_thread(HANDLE thread_handle)
{
	open_init_log();
	init_log("Started main_capture_thread");

	if (!init_hook(thread_handle)) {
		init_log("init_hook failed");
		close_init_log();
		DbgOut("Failed to init hook\n");
		free_hook();
		return 0;
	}

	init_log("Starting capture loop");
	close_init_log();

	capture_loop();
	return 0;
}

static const char log_buffer_intro[] = "Replaying buffered messages:";
#define LOG_BUFFER_INTRO_LENGTH (sizeof(log_buffer_intro) / sizeof(char));

static void append_log(const char *message, size_t chars)
{
	if (!chars || !log_buffer_enabled)
		return;

	EnterCriticalSection(&log_buffer_cs);

	bool write_intro = !log_buffer;

	if (!log_buffer || (log_buffer_size - (log_buffer_write - log_buffer)) < chars) {
		size_t size_needed = chars;
		if (write_intro)
			size_needed += LOG_BUFFER_INTRO_LENGTH;

		size_t new_size = max(log_buffer_size * 2, log_buffer_size + size_needed);
		new_size = min(10 * 1024 * 1024, new_size);

		if (new_size <= log_buffer_size) {
			log_buffer_dropped_messages += 1;
			goto leave;
		}

		char *new_buffer = realloc(log_buffer, new_size);
		if (!new_buffer) {
			log_buffer_dropped_messages += 1;
			goto leave;
		}

		log_buffer_size = new_size;
		if (new_buffer != log_buffer) {
			log_buffer_write = log_buffer_write - log_buffer + new_buffer;
			log_buffer_write_reset = log_buffer_write_reset - log_buffer + new_buffer;
			log_buffer = new_buffer;
		}
	}

	if (write_intro) {
		strcpy(log_buffer, log_buffer_intro);
		log_buffer_write_reset = log_buffer + LOG_BUFFER_INTRO_LENGTH;
		log_buffer_write_reset -= 1;
		log_buffer_write = log_buffer_write_reset;
	}

	*log_buffer_write++ = '\n';

	memmove(log_buffer_write, message, chars);
	log_buffer_write += (chars - 1);

leave:
	LeaveCriticalSection(&log_buffer_cs);
}

static inline void hlogv(const char *format, va_list args)
{
	char message[1024] = "";
	int num = _vsprintf_p(message, 1024, format, args);
	if (num) {
		if (!ipc_pipe_client_write(&pipe, message, num + 1)) {
			ipc_pipe_client_free(&pipe);
			append_log(message, num + 1);
		}
		DbgOut(message);
		DbgOut("\n");
	}
}

void hlog(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	hlogv(format, args);
	va_end(args);
}

void hlog_hr(const char *text, HRESULT hr)
{
	LPSTR buffer = NULL;

	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, hr, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			(LPSTR)&buffer, 0, NULL);

	if (buffer) {
		hlog("%s (0x%08lX): %s", text, hr, buffer);
		LocalFree(buffer);
	} else {
		hlog("%s (0x%08lX)", text, hr);
	}
}

static inline uint64_t get_clockfreq(void)
{
	static bool have_clockfreq = false;
	static LARGE_INTEGER clock_freq;

	if (!have_clockfreq) {
		QueryPerformanceFrequency(&clock_freq);
		have_clockfreq = true;
	}

	return clock_freq.QuadPart;
}

uint64_t os_gettime_ns(void)
{
	LARGE_INTEGER current_time;
	double time_val;

	QueryPerformanceCounter(&current_time);
	time_val = (double)current_time.QuadPart;
	time_val *= 1000000000.0;
	time_val /= (double)get_clockfreq();

	return (uint64_t)time_val;
}

static inline int try_lock_shmem_tex(int id)
{
	int next = id == 0 ? 1 : 0;

	if (WaitForSingleObject(tex_mutexes[id], 0) == WAIT_OBJECT_0) {
		return id;
	} else if (WaitForSingleObject(tex_mutexes[next], 0) == WAIT_OBJECT_0) {
		return next;
	}

	return -1;
}

static inline void unlock_shmem_tex(int id)
{
	if (id != -1) {
		ReleaseMutex(tex_mutexes[id]);
	}
}

static inline bool init_shared_info(size_t size)
{
	char name[64];
	sprintf_s(name, 64, "%s%u", SHMEM_TEXTURE, ++shmem_id_counter);

	shmem_file_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
			PAGE_READWRITE, 0, (DWORD)size, name);
	if (!shmem_file_handle) {
		hlog("init_shared_info: Failed to create shared memory: %d",
				GetLastError());
		return false;
	}

	shmem_info = MapViewOfFile(shmem_file_handle, FILE_MAP_ALL_ACCESS,
			0, 0, size);
	if (!shmem_info) {
		hlog("init_shared_info: Failed to map shared memory: %d",
				GetLastError());
		return false;
	}

	return true;
}

bool capture_init_shtex(struct shtex_data **data, HWND window,
		uint32_t base_cx, uint32_t base_cy, uint32_t cx, uint32_t cy,
		uint32_t format, bool flip, uintptr_t handle)
{
	if (!init_shared_info(sizeof(struct shtex_data))) {
		hlog("capture_init_shtex: Failed to initialize memory");
		return false;
	}

	*data = shmem_info;
	(*data)->tex_handle = (uint32_t)handle;

	global_hook_info->window = (uint32_t)(uintptr_t)window;
	global_hook_info->type = CAPTURE_TYPE_TEXTURE;
	global_hook_info->format = format;
	global_hook_info->flip = flip;
	global_hook_info->map_id = shmem_id_counter;
	global_hook_info->map_size = sizeof(struct shtex_data);
	global_hook_info->cx = cx;
	global_hook_info->cy = cy;
	global_hook_info->base_cx = base_cx;
	global_hook_info->base_cy = base_cy;

	if (!SetEvent(signal_ready)) {
		hlog("capture_init_shtex: Failed to signal ready: %d",
				GetLastError());
		return false;
	}

	active = true;
	return true;
}

static DWORD CALLBACK copy_thread(LPVOID unused)
{
	uint32_t pitch = thread_data.pitch;
	uint32_t cy = thread_data.cy;
	HANDLE events[2] = {NULL, NULL};
	int shmem_id = 0;

	if (!duplicate_handle(&events[0], thread_data.copy_event)) {
		hlog_hr("copy_thread: Failed to duplicate copy event: %d",
				GetLastError());
		return 0;
	}

	if (!duplicate_handle(&events[1], thread_data.stop_event)) {
		hlog_hr("copy_thread: Failed to duplicate stop event: %d",
				GetLastError());
		goto finish;
	}

	for (;;) {
		int copy_tex;
		void *cur_data;

		DWORD ret = WaitForMultipleObjects(2, events, false, INFINITE);
		if (ret != WAIT_OBJECT_0) {
			break;
		}

		EnterCriticalSection(&thread_data.data_mutex);
		copy_tex = thread_data.cur_tex;
		cur_data = thread_data.cur_data;
		LeaveCriticalSection(&thread_data.data_mutex);

		if (copy_tex < NUM_BUFFERS && !!cur_data) {
			EnterCriticalSection(&thread_data.mutexes[copy_tex]);

			int lock_id = try_lock_shmem_tex(shmem_id);
			if (lock_id != -1) {
				memcpy(thread_data.shmem_textures[lock_id],
						cur_data, pitch * cy);

				unlock_shmem_tex(lock_id);
				((struct shmem_data*)shmem_info)->last_tex =
					lock_id;

				shmem_id = lock_id == 0 ? 1 : 0;
			}

			LeaveCriticalSection(&thread_data.mutexes[copy_tex]);
		}
	}

finish:
	for (size_t i = 0; i < 2; i++) {
		if (events[i]) {
			CloseHandle(events[i]);
		}
	}

	(void)unused;
	return 0;
}

void shmem_copy_data(size_t idx, void *volatile data)
{
	EnterCriticalSection(&thread_data.data_mutex);
	thread_data.cur_tex = (int)idx;
	thread_data.cur_data = data;
	thread_data.locked_textures[idx] = true;
	LeaveCriticalSection(&thread_data.data_mutex);

	SetEvent(thread_data.copy_event);
}

bool shmem_texture_data_lock(int idx)
{
	bool locked;
	
	EnterCriticalSection(&thread_data.data_mutex);
	locked = thread_data.locked_textures[idx];
	LeaveCriticalSection(&thread_data.data_mutex);

	if (locked) {
		EnterCriticalSection(&thread_data.mutexes[idx]);
		return true;
	}

	return false;
}

void shmem_texture_data_unlock(int idx)
{
	EnterCriticalSection(&thread_data.data_mutex);
	thread_data.locked_textures[idx] = false;
	LeaveCriticalSection(&thread_data.data_mutex);

	LeaveCriticalSection(&thread_data.mutexes[idx]);
}

static inline bool init_shmem_thread(uint32_t pitch, uint32_t cy)
{
	struct shmem_data *data = shmem_info;

	thread_data.pitch = pitch;
	thread_data.cy = cy;
	thread_data.shmem_textures[0] = (uint8_t*)data + data->tex1_offset;
	thread_data.shmem_textures[1] = (uint8_t*)data + data->tex2_offset;

	thread_data.copy_event = CreateEvent(NULL, false, false, NULL);
	if (!thread_data.copy_event) {
		hlog("init_shmem_thread: Failed to create copy event: %d",
				GetLastError());
		return false;
	}

	thread_data.stop_event = CreateEvent(NULL, true, false, NULL);
	if (!thread_data.stop_event) {
		hlog("init_shmem_thread: Failed to create stop event: %d",
				GetLastError());
		return false;
	}

	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		InitializeCriticalSection(&thread_data.mutexes[i]);
	}

	InitializeCriticalSection(&thread_data.data_mutex);

	thread_data.copy_thread = CreateThread(NULL, 0, copy_thread, NULL, 0,
			NULL);
	if (!thread_data.copy_thread) {
		hlog("init_shmem_thread: Failed to create thread: %d",
				GetLastError());
		return false;
	}
	return true;
}

#ifndef ALIGN
#define ALIGN(bytes, align) (((bytes) + ((align) - 1)) & ~((align) - 1))
#endif

bool capture_init_shmem(struct shmem_data **data, HWND window,
		uint32_t base_cx, uint32_t base_cy, uint32_t cx, uint32_t cy,
		uint32_t pitch, uint32_t format, bool flip)
{
	uint32_t  tex_size       = cy * pitch;
	uint32_t  aligned_header = ALIGN(sizeof(struct shmem_data), 32);
	uint32_t  aligned_tex    = ALIGN(tex_size, 32);
	uint32_t  total_size     = aligned_header + aligned_tex * 2;
	uintptr_t align_pos;

	if (!init_shared_info(total_size)) {
		hlog("capture_init_shmem: Failed to initialize memory");
		return false;
	}

	*data = shmem_info;

	/* to ensure fast copy rate, align texture data to 256bit addresses */
	align_pos =  (uintptr_t)shmem_info;
	align_pos += aligned_header;
	align_pos &= ~(32 - 1);
	align_pos -= (uintptr_t)shmem_info;

	(*data)->last_tex = -1;
	(*data)->tex1_offset = (uint32_t)align_pos;
	(*data)->tex2_offset = (*data)->tex1_offset + aligned_tex;

	global_hook_info->window = (uint32_t)(uintptr_t)window;
	global_hook_info->type = CAPTURE_TYPE_MEMORY;
	global_hook_info->format = format;
	global_hook_info->flip = flip;
	global_hook_info->map_id = shmem_id_counter;
	global_hook_info->map_size = total_size;
	global_hook_info->pitch = pitch;
	global_hook_info->cx = cx;
	global_hook_info->cy = cy;
	global_hook_info->base_cx = base_cx;
	global_hook_info->base_cy = base_cy;

	if (!init_shmem_thread(pitch, cy)) {
		return false;
	}

	if (!SetEvent(signal_ready)) {
		hlog("capture_init_shmem: Failed to signal ready: %d",
				GetLastError());
		return false;
	}

	active = true;
	return true;
}

static inline void thread_data_free(void)
{
	if (thread_data.copy_thread) {
		DWORD ret;

		SetEvent(thread_data.stop_event);
		ret = WaitForSingleObject(thread_data.copy_thread, 500);
		if (ret != WAIT_OBJECT_0)
			TerminateThread(thread_data.copy_thread, (DWORD)-1);

		CloseHandle(thread_data.copy_thread);
	}
	if (thread_data.stop_event)
		CloseHandle(thread_data.stop_event);
	if (thread_data.copy_event)
		CloseHandle(thread_data.copy_event);
	for (size_t i = 0; i < NUM_BUFFERS; i++)
		DeleteCriticalSection(&thread_data.mutexes[i]);

	DeleteCriticalSection(&thread_data.data_mutex);

	memset(&thread_data, 0, sizeof(thread_data));
}

void capture_free(void)
{
	thread_data_free();

	if (shmem_info) {
		UnmapViewOfFile(shmem_info);
		shmem_info = NULL;
	}

	close_handle(&shmem_file_handle);

	SetEvent(signal_restart);
	active = false;

	if (overlay_info.reset)
		overlay_info.reset();
}

BOOL CALLBACK init_critical_section(INIT_ONCE *init_once, VOID *parameter, VOID **context)
{
	(void)init_once;
	(void)parameter;
	(void)context;

	InitializeCriticalSection(&log_buffer_cs);
	return true;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID unused1)
{
	if (reason == DLL_PROCESS_ATTACH) {
		wchar_t name[MAX_PATH];

		dll_inst = hinst;

		HANDLE cur_thread = OpenThread(THREAD_ALL_ACCESS, false,
				GetCurrentThreadId());

		/* this prevents the library from being automatically unloaded
		 * by the next FreeLibrary call */
		GetModuleFileNameW(hinst, name, MAX_PATH);
		LoadLibraryW(name);

		if (InitOnceExecuteOnce(&log_buffer_cs_init, init_critical_section, NULL, NULL))
			log_buffer_enabled = true;


		capture_thread = CreateThread(NULL, 0,
				(LPTHREAD_START_ROUTINE)main_capture_thread,
				(LPVOID)cur_thread, 0, 0);
		if (!capture_thread) {
			CloseHandle(cur_thread);
			return false;
		}

	} else if (reason == DLL_PROCESS_DETACH) {
		if (capture_thread) {
			stop_loop = true;
			WaitForSingleObject(capture_thread, 300);
			CloseHandle(capture_thread);
		}

		free_hook();
	}

	(void)unused1;
	return true;
}

__declspec(dllexport) LRESULT CALLBACK dummy_debug_proc(int code,
		WPARAM wparam, LPARAM lparam)
{
	static bool hooking = true;
	MSG *msg = (MSG*)lparam;

	if (hooking && msg->message == (WM_USER + 432)) {
		HMODULE user32 = GetModuleHandleW(L"USER32");
		BOOL (WINAPI *unhook_windows_hook_ex)(HHOOK) = NULL;

		unhook_windows_hook_ex = get_obfuscated_func(user32,
				"VojeleY`bdgxvM`hhDz",
				0x7F55F80C9EE3A213ULL);

		if (unhook_windows_hook_ex)
			unhook_windows_hook_ex((HHOOK)msg->lParam);
		hooking = false;
	}

	return CallNextHookEx(0, code, wparam, lparam);
}
