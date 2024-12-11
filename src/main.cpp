#define SDL_MAIN_HANDLED
#define _USE_MATH_DEFINES
#include <cmath>
#include <bhas.h>
#include <ez.hpp>
#include <immer/flex_vector.hpp>
#include <iostream>
#include <mutex>
#include <random>
#include <ranges>
#include <readerwriterqueue.h>
#include <SDL.h>
#include <string>
#include <thread>

namespace keysine {

static constexpr auto ATTACK_TIME   = std::chrono::milliseconds(10);
static constexpr auto DECAY_TIME    = std::chrono::milliseconds(500);
static constexpr auto RELEASE_TIME  = std::chrono::milliseconds(100);
static constexpr auto SUSTAIN_LEVEL = 0.5f;

enum class key {
	c3, cs3, d3, ds3, e3, f3, fs3, g3, gs3, a3, as3, b3,
	c4, cs4, d4, ds4, e4, f4, fs4, g4, gs4, a4, as4, b4,
	c5, cs5, d5, ds5, e5, 
};

enum class note_input_state { on, off }; 
enum class note_stage       { attack, decay, sustain, release, off };

struct audio_service {
	uint64_t counter = 0;
};

struct note_service {
	float energy;
	note_stage stage;
};

struct stream_info {
	bhas::channel_count output_channel_count;
};

struct note {
	int64_t id;
	float freq;
	note_input_state input_state = note_input_state::on;
	size_t service;
};

struct audio_model {
	immer::flex_vector<note> notes;
};

struct input {
	struct note_off { key k; };
	struct note_on { key k; };
	struct reset {};
	using var = std::variant<note_off, note_on, reset>;
	using buf = std::vector<var>;
	std::mutex mutex;
	std::vector<var> queue;
	buf buffer;
};

struct response {
	static constexpr auto QUEUE_SIZE = 1024;
	struct note_finished { int64_t id; };
	struct warn_queue_full { size_t size_approx; };
	using var     = std::variant<note_finished, warn_queue_full>;
	using queue_t = moodycamel::ReaderWriterQueue<var>;
	using buf     = std::vector<var>;
	queue_t queue = queue_t(QUEUE_SIZE);
	buf buffer;
};

struct pressed_key {
	key k;
	int64_t note_id;
};

struct note_info {
	SDL_Keycode kc;
	key k;
	float freq;
};

template <typename T>
struct growing_pool {
	auto acquire() -> size_t {
		return get_free_index();
	}
	auto release(size_t index) -> void {
		free_list_.push_back(index);
	}
	auto at(size_t index) -> T& {
		return items_.at(index);
	}
private:
	auto get_free_index() -> size_t {
		if (free_list_.empty()) {
			const auto index = items_.size();
			items_.emplace_back();
			return index;
		}
		const auto index = free_list_.back();
		free_list_.pop_back();
		return index;
	}
	std::vector<T> items_;
	std::vector<size_t> free_list_;
};

static constexpr note_info NOTE_INFO_TABLE[] = {
	{ SDLK_z,  key::c3,  130.8f},
	{ SDLK_s,  key::cs3, 138.6f},
	{ SDLK_x,  key::d3,  146.8f},
	{ SDLK_d,  key::ds3, 155.6f},
	{ SDLK_c,  key::e3,  164.8f},
	{ SDLK_v,  key::f3,  174.6f},
	{ SDLK_g,  key::fs3, 185.0f},
	{ SDLK_b,  key::g3,  196.0f},
	{ SDLK_h,  key::gs3, 207.7f},
	{ SDLK_n,  key::a3,  220.0f},
	{ SDLK_j,  key::as3, 233.1f},
	{ SDLK_m,  key::b3,  246.9f},
	{ SDLK_q,  key::c4,  261.6f},
	{ SDLK_2,  key::cs4, 277.2f},
	{ SDLK_w,  key::d4,  293.7f},
	{ SDLK_3,  key::ds4, 311.1f},
	{ SDLK_e,  key::e4,  329.6f},
	{ SDLK_r,  key::f4,  349.2f},
	{ SDLK_5,  key::fs4, 370.0f},
	{ SDLK_t,  key::g4,  392.0f},
	{ SDLK_6,  key::gs4, 415.3f},
	{ SDLK_y,  key::a4,  440.0f},
	{ SDLK_7,  key::as4, 466.2f},
	{ SDLK_u,  key::b4,  493.9f},
	{ SDLK_i,  key::c5,  523.3f},
	{ SDLK_9,  key::cs5, 554.4f},
	{ SDLK_o,  key::d5,  587.3f},
	{ SDLK_0,  key::ds5, 622.3f},
	{ SDLK_p,  key::e5,  659.3f},
};

// Stuff that will only be accessed by the audio thread
namespace audio {
	audio_service service;
};

// Stuff that will only be accessed by the main thread
namespace main {
	keysine::input input;
	int64_t next_id = 1;
	std::vector<pressed_key> pressed_keys;
};

// Stuff that can be accessed from any thread
namespace mt {
	ez::sync<keysine::audio_model> audio_model;
	keysine::response responder;
	keysine::stream_info stream_info;
	growing_pool<note_service> note_services;
};

auto report_(const bhas::error& item) -> void   { std::cout << "bhas error: " << item.value << std::endl; } 
auto report_(const bhas::info& item) -> void    { std::cout << "bhas info: " << item.value << std::endl; } 
auto report_(const bhas::warning& item) -> void { std::cout << "bhas warning: " << item.value << std::endl; } 
auto report(const bhas::log_item& item) -> void { std::visit([](const auto& item) { report_(item); }, item); } 
auto bhas_report(const bhas::log& log) -> void  { for (const auto& item : log) { report(item); } }
auto on_stream_start_failure() -> void                    { std::cout << "Stream start failure\n"; } 
auto on_stream_start_success(bhas::stream stream) -> void { std::cout << "Stream started\n"; } 
auto on_stream_stopped() -> void                          { std::cout << "Stream stopped\n"; }

auto on_stream_starting(bhas::stream stream) -> void {
	std::cout << "Stream starting\n";
	std::cout << "Host: " << bhas::get_system().hosts[stream.host.value].name.value << "\n";
	std::cout << "Output device: " << bhas::get_system().devices[stream.output_device.value].name.value << "\n";
	std::cout << "Sample rate: " << stream.sample_rate.value << "\n";
	mt::stream_info.output_channel_count = stream.num_output_channels;
}

auto send(ez::audio_t, response::var r) -> void {
	if (!mt::responder.queue.try_enqueue(r)) {
		static auto warned = false;
		if (!warned) {
			mt::responder.queue.enqueue(response::warn_queue_full{mt::responder.queue.size_approx()});
			warned = true;
		}
		mt::responder.queue.enqueue(r);
	}
}

auto process_note_attack(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	s.energy += 1.0f / (sr.value * float(ATTACK_TIME.count()) / 1000.0f);
	if (s.energy > 1.0f) {
		s.energy = 1.0f;
		s.stage = note_stage::decay;
	}
	return s.energy * sine;
}

auto process_note_decay(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	s.energy -= 1.0f / (sr.value * float(DECAY_TIME.count()) / 1000.0f);
	if (s.energy < SUSTAIN_LEVEL) {
		s.energy = SUSTAIN_LEVEL;
		s.stage  = note_stage::sustain;
	}
	return s.energy * sine;
}

auto process_note_sustain(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	return s.energy * sine;
}

auto process_note_release(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	s.energy -= 1.0f / (sr.value * float(RELEASE_TIME.count()) / 1000.0f);
	if (s.energy < 0.0f) {
		s.energy = 0.0f;
		s.stage  = note_stage::off;
		send(ez::audio, response::note_finished{note.id});
	}
	return s.energy * sine;
}

auto process_note_on(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	switch (s.stage) {
		case note_stage::attack:  { return process_note_attack(ez::audio, note, sine, sr); }
		case note_stage::decay:   { return process_note_decay(ez::audio, note, sine, sr); }
		case note_stage::sustain: { return process_note_sustain(ez::audio, note, sine, sr); }
		case note_stage::release: { return process_note_release(ez::audio, note, sine, sr); }
		default:                  { return 0.0f; }
	}
}

auto process_note_off(ez::audio_t, const note& note, float sine, bhas::sample_rate sr) -> float {
	auto& s = mt::note_services.at(note.service);
	switch (s.stage) {
		case note_stage::attack:
		case note_stage::decay:
		case note_stage::sustain: { s.stage = note_stage::release; [[fallthrough]]; }
		case note_stage::release: { return process_note_release(ez::audio, note, sine, sr); }
		default:                  { return 0.0f; }
	}
}

auto process_note(ez::audio_t, const note& note, uint64_t counter, bhas::sample_rate sr) -> double {
	auto sine = std::sin(2.0f * float(M_PI) * note.freq * counter / sr.value);
	switch (note.input_state) {
		case note_input_state::on: { return process_note_on(ez::audio, note, sine, sr); }
		default:                   { return process_note_off(ez::audio, note, sine, sr); }
	}
}

auto audio_callback(bhas::input_buffer input, bhas::output_buffer output, bhas::frame_count frame_count, bhas::sample_rate sample_rate, bhas::output_latency output_latency, const bhas::time_info* time_info) -> bhas::callback_result {
	// Note: I would usually be using a DSP library such as madronalib
	// here but this project is not intended to teach anything about DSP.
	// https://github.com/madronalabs/madronalib
	static constexpr auto AMP = 0.333;
	const auto m = mt::audio_model.read(ez::audio);
	for (uint32_t j = 0; j < frame_count.value; ++j) {
		float signal = 0.0f;
		for (const auto& note : m->notes) {
			signal += process_note(ez::audio, note, audio::service.counter, sample_rate);
		}
		signal *= AMP;
		for (uint32_t channel = 0; channel < mt::stream_info.output_channel_count.value; ++channel) {
			output.buffer[channel][j] = signal;
		}
		audio::service.counter++;
	}
	return bhas::callback_result::continue_;
}

auto make_callbacks() -> bhas::callbacks {
	bhas::callbacks cb;
	cb.audio                = audio_callback;
	cb.report               = bhas_report;
	cb.stream_starting      = on_stream_starting;
	cb.stream_start_failure = on_stream_start_failure;
	cb.stream_start_success = on_stream_start_success;
	cb.stream_stopped       = on_stream_stopped;
	return cb;
}

auto get_inputs(ez::main_t, std::vector<input::var>* buf) -> const std::vector<input::var>& {
	std::lock_guard lock(main::input.mutex);
	*buf = main::input.queue;
	main::input.queue.clear();
	return *buf;
}

auto get_responses(ez::main_t, std::vector<response::var>* buf) -> const std::vector<response::var>& {
	buf->clear();
	response::var r;
	while (mt::responder.queue.try_dequeue(r)) {
		buf->push_back(r);
	}
	return *buf;
}

auto get_freq(key k) -> float {
	auto match = [k](const note_info& info) { return info.k == k; };
	auto pos = std::ranges::find_if(NOTE_INFO_TABLE, match);
	if (pos != std::end(NOTE_INFO_TABLE)) {
		return pos->freq;
	}
	throw std::runtime_error("Invalid key");
}

auto get_key(SDL_Keycode c) -> std::optional<key> {
	auto match = [c](const note_info& info) { return info.kc == c; };
	auto pos = std::ranges::find_if(NOTE_INFO_TABLE, match);
	if (pos != std::end(NOTE_INFO_TABLE)) {
		return pos->k;
	}
	return std::nullopt;
}

auto reset(note_service* service) -> void {
	service->energy = 0.0f;
	service->stage = note_stage::attack;
}

auto add_note(ez::main_t, audio_model&& a, int64_t note_id, key k) -> audio_model {
	auto new_note = note{};
	new_note.id      = note_id;
	new_note.freq    = get_freq(k);
	new_note.service = mt::note_services.acquire();
	reset(&mt::note_services.at(new_note.service));
	a.notes = a.notes.push_back(new_note);
	return a;
}

auto stop_note(ez::main_t, audio_model&& a, uint64_t id) -> audio_model {
	auto match = [id](const note& n) { return n.id == id; };
	auto pos = std::ranges::find_if(a.notes, match);
	if (pos != a.notes.end()) {
		auto note = *pos;
		note.input_state = note_input_state::off;
		a.notes = a.notes.set(pos.index(), note);
	}
	return a;
}

auto remove_note(ez::main_t, audio_model&& a, uint64_t id) -> audio_model {
	auto match = [id](const note& n) { return n.id == id; };
	auto pos = std::ranges::find_if(a.notes, match);
	if (pos != a.notes.end()) {
		const auto& note = *pos;
		mt::note_services.release(note.service);
		a.notes = a.notes.erase(pos.index());
	}
	return a;
}

auto clear_notes(ez::main_t, audio_model&& a) -> audio_model {
	for (size_t i = 0; i < a.notes.size(); i++) {
		auto note = a.notes[i];
		note.input_state = note_input_state::off;
		a.notes = a.notes.set(i, note);
	}
	return a;
}

auto warn_response_queue_full(ez::main_t, size_t size_approx) -> void {
	std::cout << "Warning: response queue reached maximum capacity and had to be enlarged in the audio thread!\n";
	std::cout << "The approximate size of the response queue was: " << size_approx << "\n";
	std::cout << "Consider increasing the initial size of the queue or investigating to see if you have a bug where superfluous responses are being generated.\n";
	std::cout << "This warning will only be printed once.\n";
}

auto process_input_(ez::main_t, const input::note_off& msg) -> bool {
	auto match = [k = msg.k](const pressed_key& pk) { return pk.k == k; };
	auto pos = std::ranges::find_if(main::pressed_keys, match);
	if (pos == main::pressed_keys.end()) {
		return false;
	}
	const auto note_id = pos->note_id;
	main::pressed_keys.erase(pos);
	mt::audio_model.update(ez::main, [note_id](audio_model&& a) { return stop_note(ez::main, std::move(a), note_id); });
	return true;
} 

auto process_input_(ez::main_t, const input::note_on& msg) -> bool {
	auto match = [k = msg.k](const pressed_key& pk) { return pk.k == k; };
	if (std::ranges::find_if(main::pressed_keys, match) != main::pressed_keys.end()) {
		return false;
	}
	auto note_id = main::next_id++;
	main::pressed_keys.push_back(pressed_key{msg.k, note_id});
	mt::audio_model.update(ez::main, [note_id, k = msg.k](audio_model&& a){ return add_note(ez::main, std::move(a), note_id, k); });
	return true;
} 

auto process_input_(ez::main_t, const input::reset& msg) -> bool {
	mt::audio_model.update(ez::main, [](audio_model&& a){
		return clear_notes(ez::main, std::move(a));
	});
	return true;
} 

auto process_response_(ez::main_t, const response::note_finished& r) -> bool {
	mt::audio_model.update(ez::main, [r](audio_model&& a) {
		return remove_note(ez::main, std::move(a), r.id);
	});
	return true;
}

auto process_response_(ez::main_t, const response::warn_queue_full& r) -> bool {
	warn_response_queue_full(ez::main, r.size_approx);
	return false;
}

auto process_input(ez::main_t, const input::var& i) -> bool       { return std::visit([](const auto& i) { return process_input_(ez::main, i); }, i); } 
auto process_response(ez::main_t, const response::var& r) -> bool { return std::visit([](const auto& r) { return process_response_(ez::main, r); }, r); }

auto process_inputs(ez::main_t, const input::buf& buf) -> bool {
	if (buf.empty()) {
		return false;
	}
	auto model_changed = false;
	for (const auto& msg : buf) {
		model_changed |= process_input(ez::main, msg);
	}
	return model_changed;
}

auto process_responses(ez::main_t, const response::buf& buf) -> bool {
	if (buf.empty()) {
		return false;
	}
	auto model_changed = false;
	for (const auto& r : buf) {
		model_changed |= process_response(ez::main, r);
	}
	return model_changed;
}

auto process_inputs(ez::main_t) -> bool    { return process_inputs(ez::main, get_inputs(ez::main, &main::input.buffer)); }
auto process_responses(ez::main_t) -> bool { return process_responses(ez::main, get_responses(ez::main, &mt::responder.buffer)); }

auto push(ez::main_t, input::var&& msg) -> void {
	std::lock_guard lock(main::input.mutex);
	main::input.queue.push_back(std::move(msg));
}

auto on_key_down(ez::main_t, SDL_KeyboardEvent event, std::stop_source* stop) -> void {
	if (event.repeat) {
		return;
	}
	switch (event.keysym.sym) {
		case SDLK_ESCAPE: { stop->request_stop(); return; }
		case SDLK_SPACE:  { push(ez::main, input::reset{}); return; }
		default:          { if (const auto k = get_key(event.keysym.sym)) { push(ez::main, input::note_on{*k}); } }
	}
}

auto on_key_up(ez::main_t, SDL_KeyboardEvent event, std::stop_source* stop) -> void {
	if (const auto k = get_key(event.keysym.sym)) {
		push(ez::main, input::note_off{*k});
	}
}

auto handle_event(ez::main_t, const SDL_Event& event, std::stop_source* stop) -> void {
	switch (event.type) {
		case SDL_KEYDOWN: { on_key_down(ez::main, event.key, stop); break; }
		case SDL_KEYUP:   { on_key_up(ez::main, event.key, stop); break; }
		case SDL_QUIT:    { stop->request_stop(); break; }
		default:          { break; }
	}
}

auto handle_events(ez::main_t, std::stop_source* stop) -> void {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		handle_event(ez::main, event, stop);
	}
}

auto main_loop(ez::main_t) -> void {
	static constexpr auto FRAME_INTERVAL = std::chrono::milliseconds(10);
	static constexpr auto GC_INTERVAL    = std::chrono::seconds(1);
	auto now        = std::chrono::steady_clock::now();
	auto next_gc    = now;
	auto next_frame = now;
	auto stop_source = std::stop_source{};
	auto stop_token  = stop_source.get_token();
	while (!stop_token.stop_requested()) {
		auto now = std::chrono::steady_clock::now();
		auto audio_model_changed = false;
		handle_events(ez::main, &stop_source);
		bhas::update();
		audio_model_changed |= process_inputs(ez::main);
		audio_model_changed |= process_responses(ez::main);
		if (audio_model_changed) {
			mt::audio_model.publish(ez::main);
		}
		if (now > next_gc) {
			mt::audio_model.gc(ez::main);
			next_gc = now + GC_INTERVAL;
		}
		std::this_thread::sleep_until(next_frame);
		next_frame += FRAME_INTERVAL;
	}
}

auto setup(ez::main_t) -> SDL_Window* {
	bhas::init(make_callbacks());
	const auto system = bhas::get_system();
	bhas::stream_request request;
	request.input_device = system.default_input_device;
	request.output_device = system.default_output_device;
	request.sample_rate = system.devices[request.output_device.value].default_sample_rate;
	bhas::request_stream(request);
	if (SDL_Init(SDL_INIT_EVENTS) < 0) {
		throw std::runtime_error("SDL_Init failed");
	}
	const auto window =
		SDL_CreateWindow(
			"this window needs to have focus for this to work",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			500, 50, SDL_WINDOW_SHOWN);
	if (!window) {
		throw std::runtime_error("SDL_CreateWindow failed");
	}
	return window;
}

auto shutdown(ez::main_t, SDL_Window* window) -> void {
	bhas::shutdown();
	SDL_DestroyWindow(window);
	SDL_Quit();
}

auto main_thread(ez::main_t) -> void {
	const auto window = setup(ez::main);
	std::cout << "Type letters and numbers on QWERTY keyboard to play notes.\n";
	std::cout << "Press spacebar to stop all notes.\n";
	std::cout << "Type ESC to exit.\n";
	main_loop(ez::main);
	shutdown(ez::main, window);
}

} // keysine

auto main(int argc, const char* argv[]) -> int {
	try {
		keysine::main_thread(ez::main);
		return EXIT_SUCCESS;
	}
	catch (const std::exception& e) { std::cerr << "Error: " << e.what() << std::endl; }
	catch (...)                     { std::cerr << "Unknown error" << std::endl; }
	return EXIT_FAILURE;
}