/*
 * RommPS - its program, made from the PS5 Vulkan Template's UI starter.
 *
 * One complete design of PS5_VKHomebrewUI's kit (ps5/ui/kit.hpp) fills the
 * screen: its procedural backdrop, its scene, its frosted overlay and its post
 * overlay, its sounds on the console's audio output, its rumble and its light
 * bar colour. There is no switcher: every button is the design's, and holding
 * OPTIONS for a second leaves (back to the menu, or out of a title of one
 * program). The design grew from the kit's "aurora", in kit/screen.cpp.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kit.hpp"

// Covers become textures through the kit's renderer (kit/rommps_app.cpp). C
// linkage, so it names the same function from inside this file's namespace.
extern "C" void rommps_set_renderer(void *renderer);

// RommPS's own interface sounds: assets/rommps/sounds/<cue>_NN.wav in the
// title (platform/ps5/app/assets/sounds in the repository; focus_01.wav,
// select_01.wav, back_01.wav, ...: the kit's cue names, audio/cues.cpp), 48 kHz
// 16- or 24-bit PCM. Each cue there replaces the kit's recordings of that cue;
// the cues it leaves out keep them.
std::string soundPackDir(const ps5ui::Kit &kit)
{
	const size_t slash = kit.assets.rfind('/');
	return (slash == std::string::npos ? std::string(".") : kit.assets.substr(0, slash)) + "/rommps/sounds";
}

bool cueOfFile(const std::string &file, hui::audio::Cue *cue)
{
	if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".wav") != 0) {
		return false;
	}
	std::string stem = file.substr(0, file.size() - 4);
	const size_t underscore = stem.rfind('_');
	if (underscore != std::string::npos && underscore + 1 < stem.size() &&
		stem.find_first_not_of("0123456789", underscore + 1) == std::string::npos) {
		stem.resize(underscore);
	}
	for (int i = 0; i < (int)hui::audio::Cue::count; i++) {
		if (stem == hui::audio::cue_name((hui::audio::Cue)i)) {
			*cue = (hui::audio::Cue)i;
			return true;
		}
	}
	return false;
}

// Rebuilds the kit's sound bank with the pack's cues in place of the built-in ones.
void loadSoundPack(ps5ui::Kit &kit)
{
	using namespace hui::audio;
	const std::string packDir = soundPackDir(kit);
	std::vector<std::string> files = hui::save::list_files(packDir);
	if (files.empty()) {
		return;
	}
	std::sort(files.begin(), files.end());
	SoundBank bank;
	bool replaced[(int)Cue::count] = {};
	int loaded = 0, rejected = 0;
	for (const std::string &name : files) {
		Cue cue;
		std::string data;
		if (!cueOfFile(name, &cue) || !hui::save::read_file(packDir + "/" + name, &data, 16u << 20)) {
			rejected++;
			continue;
		}
		DecodedWav wav = decode_wav(data);
		if (!wav.ok()) {
			say("sound pack: %s: %s", name.c_str(), wav.error.c_str());
			rejected++;
			continue;
		}
		// Both sets, so a cue sounds the same whichever set asks for it
		for (int set = 0; set < (int)SoundSet::count; set++) {
			bank.add((SoundSet)set, cue, wav.samples, wav.frames);
		}
		replaced[(int)cue] = true;
		loaded++;
	}
	if (loaded == 0) {
		say("sound pack: nothing usable in %s (%d rejected)", packDir.c_str(), rejected);
		return;
	}
	// The built-in recordings of the cues the pack leaves out
	for (int set = 0; set < (int)SoundSet::count; set++) {
		const std::string dir = kit.assets + "/audio/sfx/" + sound_set_name((SoundSet)set);
		for (const std::string &name : hui::save::list_files(dir)) {
			Cue cue;
			std::string data;
			if (!cueOfFile(name, &cue) || replaced[(int)cue] || !hui::save::read_file(dir + "/" + name, &data, 16u << 20)) {
				continue;
			}
			DecodedWav wav = decode_wav(data);
			if (wav.ok()) {
				bank.add((SoundSet)set, cue, std::move(wav.samples), wav.frames);
			}
		}
	}
	kit.sounds = std::move(bank);
	say("sound pack: %d sounds from %s (%d rejected)", loaded, packDir.c_str(), rejected);
}

// The design on screen: this title's own, in kit/screen.cpp
std::unique_ptr<hui::app::Concept> makeScreen(hui::app::Context &context)
{
	return ps5ui::make_screen(context);
}

class VulkanExample : public ps5ui::KitExample
{
public:
	// What a design receives: the fonts, the sample content, the live numbers
	// it may show and the settings it may change
	hui::Settings choices;
	hui::app::Telemetry telemetry;
	std::unique_ptr<hui::app::Context> context;
	std::unique_ptr<hui::app::Concept> screen;
	hui::app::Frame frame;
	hui::ui::Feedback feedback;
	ps5ui::HoldToLeave leaving;
	hui::gfx::DrawList leavingList;
	double fpsSeconds{ 0.0 };
	int fpsFrames{ 0 };

	VulkanExample() : KitExample()
	{
		title = "RommPS";
		name = "rommps";
		// The kit draws the whole screen and owns the pad
		settings.overlay = false;
		ps5.ownOverlay = true;
		defaultClearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		// The kit's sounds, without its music behind them
		prepareKit({ .music = false });
		// Before the first sound plays: the bank is replaced whole
		loadSoundPack(kit);
		rommps_set_renderer(static_cast<hui::gfx::Renderer *>(&kit.renderer));
		context = std::make_unique<hui::app::Context>(hui::app::Context{ kit.fonts, kit.catalog, telemetry, choices });
		screen = makeScreen(*context);
		screen->enter();
		kit.apply(choices);
		feedback.play(hui::audio::Cue::welcome);
		prepared = true;
	}

	// Holding OPTIONS for a second leaves; a press is the design's
	void holdOptions(float dt)
	{
		leaving.label = ps5.optionsEnds ? "Back to the menu" : "Close";
		if (!ps5.frameBudget && leaving.update(dt, (ps5_pad().held & PAD_OPTIONS) != 0)) {
			quit = true;
		}
	}

	void updateTelemetry()
	{
		if (benchmark.active) {
			// A test run's pictures must not depend on the console's timing
			telemetry.push(16.7f);
			telemetry.fps = 59.9f;
			telemetry.average_ms = 16.7f;
		} else {
			telemetry.push(frameTimer * 1000.0f);
			fpsSeconds += frameTimer;
			fpsFrames++;
			if (fpsSeconds >= 0.5) {
				telemetry.fps = (float)(fpsFrames / fpsSeconds);
				telemetry.average_ms = (float)(fpsSeconds * 1000.0 / fpsFrames);
				fpsSeconds = 0.0;
				fpsFrames = 0;
			}
			telemetry.voices = kit.mixer.active_voices();
		}
		telemetry.draw_calls = kit.renderer.last_draw_calls();
		telemetry.instances = kit.renderer.last_instances();
	}

	void render() override
	{
		if (!prepared) {
			return;
		}
		prepareFrame();
		const float dt = std::min(frameTimer, 0.05f);
		// A test run ignores the pad: the design shows its entrance and rests,
		// or takes the presses its test-run.txt gives ("press rommps <frame> r1")
		hui::InputFrame input = kit.input();
		if (ps5.frameBudget) {
			input = kit.scripted("rommps", ps5.framesDrawn);
		}
		holdOptions(dt);
		updateTelemetry();
		// CIRCLE is the screen's (back, in the setup and the sheets): RommPS closes
		// with the PS button, or by holding OPTIONS
		screen->update(input, dt, feedback);
		if (context->settings_changed) {
			context->settings_changed = false;
			kit.apply(choices);
		}
		kit.play(feedback, screen->info().sounds, choices.haptics);
		feedback.clear();
		if (choices.light_bar) {
			kit.light_bar(screen->info().accent);
		}
		kit.tick(dt);

		// backdrop, scene, [the glass copy], overlay, post: the order a design is drawn in
		frame.reset();
		frame.glass_texture = kit.renderer.glass_texture();
		screen->draw(frame);
		kit.renderer.begin();
		kit.renderer.backdrop(frame.backdrop);
		kit.renderer.draw(frame.scene);
		if (frame.glass) {
			kit.renderer.glass();
		}
		kit.renderer.draw(frame.overlay);
		kit.renderer.backdrop(frame.post);
		leavingList.clear();
		leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
		kit.renderer.draw(leavingList);
		buildKitCommandBuffer();
		submitFrame();
	}

	~VulkanExample() override
	{
		if (device) {
			vkDeviceWaitIdle(device);
		}
	}
};

VULKAN_EXAMPLE_MAIN()
