
#include <rack.hpp>
// libpd
#include "z_libpd.h"
#include "util/z_print_util.h"
// Pd-Host
#include <osdialog.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
// #include <efsw/efsw.h>
#if defined ARCH_WIN
    #include <windows.h>
#endif

using namespace rack;

Plugin* pluginInstance;

static const int N_IN_OUT = 6;
static const int MAX_BUFFER_SIZE = 4096;

struct PureData;

// Forward declaration for MIDI hook
void midiByteHook(int port, int byte);
void midiNoteHook(int channel, int pitch, int velocity);
void midiControlChangeHook(int channel, int controller, int value);
void midiProgramChangeHook(int channel, int value);
void midiPitchBendHook(int channel, int value);
void midiAfterTouchHook(int channel, int value);
void midiPolyAfterTouchHook(int channel, int pitch, int value);

struct ProcessBlock{
    float sampleRate = 0.f;
    float sampleTime = 0.f;
    int bufferSize = 1;
    float inputs[N_IN_OUT][MAX_BUFFER_SIZE] = {};
    float outputs[N_IN_OUT][MAX_BUFFER_SIZE] = {};
    float knobs[N_IN_OUT] = {};
    bool switches[N_IN_OUT] = {};
    float lights[N_IN_OUT][3] = {};
    float switchLights[N_IN_OUT][3] = {};
};

struct ScriptEngine{
    // Virtual methods for subclasses
    virtual ~ScriptEngine() {}
    /** Executes the script.
    Return nonzero if failure, and set error message with setMessage().
    Called only once per instance.
    */
    virtual int run(const std::string& path, const std::string& script) {return 0;}

    /** Calls the script's process() method.
    Return nonzero if failure, and set error message with setMessage().
    */
    virtual int process() {return 0;}

    // Communication with PureData module.
    // These cannot be called from your constructor, so initialize your engine in the run() method.
    void display(const std::string& message);
    void setFrameDivider(int frameDivider);
    void setBufferSize(int bufferSize);
    ProcessBlock* getProcessBlock();
    // private
    PureData* module = NULL;
};

static const int BUFFERSIZE = MAX_BUFFER_SIZE * N_IN_OUT;

// Thread-local storage for current engine instance being processed
static thread_local struct LibPDEngine* g_current_engine = nullptr;

static std::vector<std::string> split(const std::string& s, char delim){
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (getline(ss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

struct LibPDEngine : ScriptEngine{
    t_pdinstance* _lpd = NULL;
    int _pd_block_size = 64;
    int _sampleRate = 0;
    int _ticks = 0;
    bool _init = true;

    // Per-instance state
    float _lights[N_IN_OUT][3] = {};
    float _switchLights[N_IN_OUT][3] = {};
    std::string _utility[2] = {};
    bool _display_is_valid = false;

    float _old_knobs[N_IN_OUT] = {};
    bool  _old_switches[N_IN_OUT] = {};
    float _output[BUFFERSIZE] = {};
    float _input[BUFFERSIZE] = {};//  = (float*)malloc(1024*2*sizeof(float));
    const static std::map<std::string, int> _light_map;
    const static std::map<std::string, int> _switchLight_map;
    const static std::map<std::string, int> _utility_map;
    ~LibPDEngine() {
        if (_lpd) {
            libpd_free_instance(_lpd);
        }
    }
    void sendInitialStates(const ProcessBlock* block);
    static void send_toHost(const char* source, const char* symbol, int argc, t_atom* argv);
    bool knobChanged(const float* knobs, int idx);
    bool switchChanged(const bool* knobs, int idx);
    void sendKnob(const int idx, const float value);
    void sendSwitch(const int idx, const bool value);
    int run(const std::string& path, const std::string& script) override {
        // DEBUG
//        fprintf(stderr, ">>> LibPDEngine::run() called\n");
        ProcessBlock* block = getProcessBlock();
        _sampleRate = (int)APP->engine->getSampleRate();
        setBufferSize(_pd_block_size);
        setFrameDivider(1);
        libpd_init();
        
// Set ELSE's path
//        std::string pluginPath = asset::plugin(pluginInstance, "");
//        std::string elsePath = pluginPath + "patches/else";
        // DEBUG
//        fprintf(stderr, "ELSE path: %s\n", elsePath.c_str());
// Add ELSE to path
//        libpd_add_to_search_path(elsePath.c_str());
//        fprintf(stderr, "ELSE path set to: %s\n", elsePath.c_str());
//        fprintf(stderr, "Search path added. Check if folder exists: ");
/*        FILE* test = fopen((elsePath + "/adsr~.darwin-arm64-32.so").c_str(), "r");
        if (test) {
            fprintf(stderr, "Found adsr~.darwin-arm64-32.so\n");
            fclose(test);
        } else {
            fprintf(stderr, "NOT FOUND\n");
        }*/
        
        _lpd = libpd_new_instance();

        // Set thread-local variable to track current engine for callbacks
        g_current_engine = this;

        libpd_set_concatenated_printhook([](const char* s) {
            fprintf(stderr, "libpd: %s\n", s);
        });
        libpd_set_messagehook([](const char* source, const char* symbol, int argc, t_atom* argv) {
            send_toHost(source, symbol, argc, argv);
        });
        libpd_init_audio(N_IN_OUT, N_IN_OUT, _sampleRate);
        libpd_bind("toHost");
       
        libpd_set_midibytehook(midiByteHook);
        libpd_set_noteonhook(midiNoteHook);
        libpd_set_controlchangehook(midiControlChangeHook);
        libpd_set_programchangehook(midiProgramChangeHook);
        libpd_set_pitchbendhook(midiPitchBendHook);
        libpd_set_aftertouchhook(midiAfterTouchHook);
        libpd_set_polyaftertouchhook(midiPolyAfterTouchHook);

        // compute audio    [; pd dsp 1(
        libpd_start_message(1); // one entry in list
        libpd_add_float(1.0f);
        libpd_finish_message("pd", "dsp");
        std::string name = system::getFilename(path);
        std::string dir  = system::getDirectory(path);
        libpd_openfile(name.c_str(), dir.c_str());

        sendInitialStates(block);

        return 0;
    }
    int process() override {
        // block
        ProcessBlock* block = getProcessBlock();

        // get samples
        int rows = N_IN_OUT;
        for (int s = 0; s < _pd_block_size; s++) {
            for (int r = 0; r < rows; r++) {
                _input[s * rows + r] = block->inputs[r][s];
            }
        }

        libpd_set_instance(_lpd);
        // Set thread-local variable to track current engine for callbacks
        g_current_engine = this;
        // knobs
        for (int i = 0; i < N_IN_OUT; i++) {
            if (knobChanged(block->knobs, i)) {
                sendKnob(i, block->knobs[i]);
            }
        }
        // lights
        for (int i = 0; i < N_IN_OUT; i++) {
            block->lights[i][0] = _lights[i][0];
            block->lights[i][1] = _lights[i][1];
            block->lights[i][2] = _lights[i][2];
        }
        // switch lights
        for (int i = 0; i < N_IN_OUT; i++) {
            block->switchLights[i][0] = _switchLights[i][0];
            block->switchLights[i][1] = _switchLights[i][1];
            block->switchLights[i][2] = _switchLights[i][2];
        }
        // switches
        for (int i = 0; i < N_IN_OUT; i++) {
            if (switchChanged(block->switches, i)) {
                sendSwitch(i, block->switches[i]);
            }
        }
        // display
        if (_display_is_valid) {
            display(_utility[1]);
            _display_is_valid = false;
        }
        // process samples in libpd
        _ticks = 1;
        libpd_process_float(_ticks, _input, _output);

        // return samples
        for (int s = 0; s < _pd_block_size; s++) {
            for (int r = 0; r < rows; r++) {
                block->outputs[r][s] = _output[s * rows + r]; // scale up again to +-5V signal
                // there is a correction multiplier, because libpd's output is too quiet(?)
            }
        }
        return 0;
    }
};

bool LibPDEngine::knobChanged(const float* knobs, int i) {
    bool knob_changed = false;
    if (_old_knobs[i] != knobs[i]) {
        knob_changed = true;
        _old_knobs[i] = knobs[i];
    }
    return knob_changed;
}

bool LibPDEngine::switchChanged(const bool* switches, int i) {
    bool switch_changed = false;
    if (_old_switches[i] != switches[i]) {
        switch_changed = true;
        _old_switches[i] = switches[i];
    }
    return switch_changed;
}

const std::map<std::string, int> LibPDEngine::_light_map{
    { "L1", 0 },
    { "L2", 1 },
    { "L3", 2 },
    { "L4", 3 },
    { "L5", 4 },
    { "L6", 5 }
};

const std::map<std::string, int> LibPDEngine::_switchLight_map{
    { "S1", 0 },
    { "S2", 1 },
    { "S3", 2 },
    { "S4", 3 },
    { "S5", 4 },
    { "S6", 5 }
};

const std::map<std::string, int> LibPDEngine::_utility_map{
    { "display", 0 },
    { "error:", 1 }
};

void LibPDEngine::sendKnob(const int idx, const float value) {
    std::string knob = "K" + std::to_string(idx + 1);
    libpd_start_message(1);
    libpd_add_float(value);
    libpd_finish_message("fromHost", knob.c_str());
}

void LibPDEngine::sendSwitch(const int idx, const bool value) {
    std::string sw = "S" + std::to_string(idx + 1);
    libpd_start_message(1);
    libpd_add_float(value);
    libpd_finish_message("fromHost", sw.c_str());
}

void LibPDEngine::sendInitialStates(const ProcessBlock* block) {
    // knobs
    for (int i = 0; i < N_IN_OUT; i++) {
        sendKnob(i, block->knobs[i]);
        sendSwitch(i, block->knobs[i]);
    }

    for (int i = 0; i < N_IN_OUT; i++) {
        _lights[i][0] = 0;
        _lights[i][1] = 0;
        _lights[i][2] = 0;
        _switchLights[i][0] = 0;
        _switchLights[i][1] = 0;
        _switchLights[i][2] = 0;
    }

    //_utility[0] = "";
    //_utility[1] = "";

    //_display_is_valid = false;
}

// ------------------- plugin ------------------------------

static std::string settingsPdEditorPath =
#if defined ARCH_LIN
	"\"/usr/bin/pd-gui\"";
#else
	"";
#endif

json_t* settingsToJson() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "pdEditorPath", json_string(settingsPdEditorPath.c_str()));
	return rootJ;
}

void settingsFromJson(json_t* rootJ) {
	json_t* pdEditorPathJ = json_object_get(rootJ, "pdEditorPath");
	if (pdEditorPathJ)
		settingsPdEditorPath = json_string_value(pdEditorPathJ);
}

void settingsLoad() { // Load plugin settings
	std::string filename = asset::user("EL-LOCUS-SOLUS-Pd-Host.json");
	FILE* file = std::fopen(filename.c_str(), "r");
	if (!file) {
		return;
	}
	DEFER({
		std::fclose(file);
	});

	json_error_t error;
	json_t* rootJ = json_loadf(file, 0, &error);
	if (rootJ) {
		settingsFromJson(rootJ);
		json_decref(rootJ);
	}
}

void settingsSave() {
	json_t* rootJ = settingsToJson();

	std::string filename = asset::user("EL-LOCUS-SOLUS-Pd-Host.json");
	FILE* file = std::fopen(filename.c_str(), "w");
	if (file) {
		json_dumpf(rootJ, file, JSON_INDENT(2) | JSON_REAL_PRECISION(9));
		std::fclose(file);
	}

	json_decref(rootJ);
}

std::string getApplicationPathDialog() {
	char* pathC = NULL;
#if defined ARCH_LIN
	pathC = osdialog_file(OSDIALOG_OPEN, "/usr/bin/", NULL, NULL);
#elif defined ARCH_WIN
	osdialog_filters* filters = osdialog_filters_parse("Executable:exe");
	pathC = osdialog_file(OSDIALOG_OPEN, "C:/", NULL, filters);
	osdialog_filters_free(filters);
#elif defined ARCH_MAC
	osdialog_filters* filters = osdialog_filters_parse("Application:app");
	pathC = osdialog_file(OSDIALOG_OPEN, "/Applications/", NULL, filters);
	osdialog_filters_free(filters);
#endif
	if (!pathC)
		return "";

	std::string path = "\"";
	path += pathC;
	path += "\"";
	std::free(pathC);
	return path;
}

void setPdEditorDialog() {
	std::string path = getApplicationPathDialog();
	if (path == "")
		return;
	settingsPdEditorPath = path;
	settingsSave();
}

struct PureData : Module {
	enum ParamIds {
		ENUMS(KNOB_PARAMS, N_IN_OUT),
		ENUMS(SWITCH_PARAMS, N_IN_OUT),
		NUM_PARAMS
	};
	enum InputIds {
		ENUMS(IN_INPUTS, N_IN_OUT),
		NUM_INPUTS
	};
	enum OutputIds {
		ENUMS(OUT_OUTPUTS, N_IN_OUT),
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(LIGHT_LIGHTS, N_IN_OUT * 3),
		ENUMS(SWITCH_LIGHTS, N_IN_OUT * 3),
		NUM_LIGHTS
	};
    
    // midi in
//    midi::InputQueue midiInput;
    uint8_t runningStatus = 0;
    uint8_t dataBuffer[2] = {0, 0};
    int dataIndex = 0;
    int dataBytesNeeded = 0;
    bool inSysex = false;
    
    // midi out
//    midi::Output midiOutput;
    uint8_t midiRunningStatus = 0;
    uint8_t midiDataBuffer[2] = {0, 0};
    int midiDataIndex = 0;
    int midiDataBytesNeeded = 0;
    
	std::string message;
	std::string path;
	std::string script;
	std::mutex scriptMutex;
	ScriptEngine* scriptEngine = NULL;
	int frame = 0;
	int frameDivider;
	// This is dynamically allocated to have some protection against script bugs.
	ProcessBlock* block;
	int buf_idx = 0;

//	efsw_watcher efsw = NULL;

	/** Script that has not yet been approved to load */
	std::string unsecureScript;
	bool securityRequested = false;
	bool securityAccepted = false;
    


	PureData() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		for (int i = 0; i < N_IN_OUT; i++)
			configParam(KNOB_PARAMS + i, 0.f, 1.f, 0.5f, string::f("Knob %d", i + 1));
		for (int i = 0; i < N_IN_OUT; i++)
			configParam(SWITCH_PARAMS + i, 0.f, 1.f, 0.f, string::f("Switch %d", i + 1));
		// for (int i = 0; i < N_IN_OUT; i++)
		// 	configInput(IN_INPUTS + i, string::f("#%d", i + 1));
		// for (int i = 0; i < N_IN_OUT; i++)
		// 	configOutput(OUT_OUTPUTS + i, string::f("#%d", i + 1));

		block = new ProcessBlock;
		setPath("");
        
	}

	~PureData() {
		setPath("");
		delete block;
	}

	void onReset() override {
		setScript(script);
	}

	void process(const ProcessArgs& args) override {
//        fprintf(stderr, "process called, frame=%d, frameDivider=%d\n", frame, frameDivider);
		// Load security-sandboxed script if the security warning message is accepted.
		if (unsecureScript != "" && securityAccepted) {
			setScript(unsecureScript);
			unsecureScript = "";
		}

		// Frame divider for reducing sample rate
		if (++frame < frameDivider)
			return;
		frame = 0;

		// Clear outputs if no script is running
		if (!scriptEngine) {
			for (int i = 0; i < N_IN_OUT; i++)
				for (int c = 0; c < 3; c++)
					lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(0.f);
			for (int i = 0; i < N_IN_OUT; i++)
				for (int c = 0; c < 3; c++)
					lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(0.f);
			for (int i = 0; i < N_IN_OUT; i++)
				outputs[OUT_OUTPUTS + i].setVoltage(0.f);
			return;
		}

        // Process MIDI input
        midi::Message msg;
        while (midiInput.tryPop(&msg, args.frame)) {
            for (size_t i = 0; i < msg.bytes.size(); i++) {
                uint8_t byte = msg.bytes[i];
                
                // Send EVERY byte to raw MIDI input FIRST
                libpd_midibyte(0, byte);
                
                // Realtime messages go to sysrealtimein
                if (byte >= 0xF8) {
                    libpd_sysrealtime(0, byte);
                    continue;
                }
                
                // Status byte (0x80-0xEF)
                if (byte & 0x80) {
                    // Store running status
                    runningStatus = byte;
                    dataBytesNeeded = 0;
                    
                    if ((byte & 0xF0) == 0xC0 || (byte & 0xF0) == 0xD0) {
                        dataBytesNeeded = 1;
                    } else if ((byte & 0xF0) >= 0x80 && (byte & 0xF0) <= 0xE0) {
                        dataBytesNeeded = 2;
                    } else if (byte == 0xF0) { // Sysex start
                        inSysex = true;
                        libpd_sysex(0, byte);
                        continue;
                    } else if (byte == 0xF7) { // Sysex end
                        inSysex = false;
                        libpd_sysex(0, byte);
                        continue;
                    } else {
                        // Other system messages (F1-F6) already sent via libpd_midibyte above
                        continue;
                    }
                    
                    dataBuffer[0] = 0;
                    dataBuffer[1] = 0;
                    dataIndex = 0;
                }
                // Data byte
                else {
                    if (inSysex) {
                        // Already sent via libpd_midibyte above
                        continue;
                    }
                    
                    if (runningStatus == 0) {
                        continue;
                    }
                    
                    dataBuffer[dataIndex++] = byte;
                    
                    if (dataIndex >= dataBytesNeeded) {
                        int status = runningStatus;
                        int channel = status & 0x0F;
                        int type = status & 0xF0;
                        
                        if (type == 0x80) {
                            libpd_noteon(channel, dataBuffer[0], 0);
                        } else if (type == 0x90) {
                            libpd_noteon(channel, dataBuffer[0], dataBuffer[1]);
                        } else if (type == 0xA0) {
                            libpd_polyaftertouch(channel, dataBuffer[0], dataBuffer[1]);
                        } else if (type == 0xB0) {
                            libpd_controlchange(channel, dataBuffer[0], dataBuffer[1]);
                        } else if (type == 0xC0) {
                            libpd_programchange(channel, dataBuffer[0]);
                        } else if (type == 0xD0) {
                            libpd_aftertouch(channel, dataBuffer[0]);
                        } else if (type == 0xE0) {
                            int value = (dataBuffer[1] << 7) | dataBuffer[0];
                            libpd_pitchbend(channel, value - 8192);
                        }
                        
                        dataIndex = 0;
                    }
                }
            }
        }
// Inputs
//        for(int i = 0; i < N_IN_OUT; i++)
//            block->inputs[i][buf_idx] = inputs[IN_INPUTS + i].getVoltage();
        for(int i = 0; i < N_IN_OUT; i++){
            int ch = inputs[i].getChannels();
            if(ch > 1)
                ch = 1;
            for(int c = 0; c < 1; c++){
                if(c < ch)
                    block->inputs[i*1+c][buf_idx] = inputs[i].getVoltage(c);
                else
                    block->inputs[i*1+c][buf_idx] = 0.f;
            }
        }
// Process block
		if (++buf_idx >= block->bufferSize) {
			std::lock_guard<std::mutex> lock(scriptMutex);
			buf_idx = 0;

			// Block settings
			block->sampleRate = args.sampleRate;
			block->sampleTime = args.sampleTime;

			// Params
			for (int i = 0; i < N_IN_OUT; i++)
				block->knobs[i] = params[KNOB_PARAMS + i].getValue();
			for (int i = 0; i < N_IN_OUT; i++)
				block->switches[i] = params[SWITCH_PARAMS + i].getValue() > 0.f;
			float oldKnobs[N_IN_OUT];
			std::memcpy(oldKnobs, block->knobs, sizeof(oldKnobs));

			// Run ScriptEngine's process function
			{
				// Process buffer
				if (scriptEngine) {
					if (scriptEngine->process()) {
						WARN("Patch %s process() failed. Stopped script.", path.c_str());
						delete scriptEngine;
						scriptEngine = NULL;
						return;
					}
				}
			}

			// Params
			// Only set params if values were changed by the script. This avoids issues when the user is manipulating them from the UI thread.
			for (int i = 0; i < N_IN_OUT; i++) {
				if (block->knobs[i] != oldKnobs[i])
					params[KNOB_PARAMS + i].setValue(block->knobs[i]);
			}
			// Lights
			for (int i = 0; i < N_IN_OUT; i++)
				for (int c = 0; c < 3; c++)
					lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(block->lights[i][c]);
			for (int i = 0; i < N_IN_OUT; i++)
				for (int c = 0; c < 3; c++)
					lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(block->switchLights[i][c]);
		}

// Outputs
//		for (int i = 0; i < N_IN_OUT; i++)
//			outputs[OUT_OUTPUTS + i].setVoltage(block->outputs[i][buf_idx]);
        for(int i = 0; i < N_IN_OUT; i++){
            int ch = inputs[IN_INPUTS+i].getChannels();
            if(ch > 1)
                ch = 1;
            for(int c = 0; c < 1; c++){
                float v = 0.f;
                if(c < ch)
                    v = block->outputs[i*1+c][buf_idx];
                outputs[OUT_OUTPUTS + i].setVoltage(v, c);
            }
            outputs[OUT_OUTPUTS + i].setChannels(ch);
        }
	}

	void setPath(std::string path) {
		// Cleanup
/*		if (efsw) {
			efsw_release(efsw);
			efsw = NULL;
		}*/
		this->path = "";
		setScript("");

		if (path == "")
			return;

		this->path = path;
		loadPath();

		if (this->script == "")
			return;

// Watch file
        // Old V1 code that causes  error
//        std::string dir = string::directory(path);
        // Correct V2 code, verified by the official API documentation
        std::string dir = rack::system::getDirectory(path);
        
//		efsw = efsw_create(false);
//		efsw_addwatch(efsw, dir.c_str(), watchCallback, false, this);
//		efsw_watch(efsw);
	}

	void loadPath() {
		// Read file
		std::ifstream file;
		file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
		try {
			file.open(path);
			std::stringstream buffer;
			buffer << file.rdbuf();
			std::string script = buffer.str();
			setScript(script);
		}
		catch (const std::runtime_error& err) {
			// Fail silently
		}
	}
    
    void setScript(std::string script) {
        std::lock_guard<std::mutex> lock(scriptMutex);
        // Reset script state
        if (scriptEngine) {
            delete scriptEngine;
            scriptEngine = NULL;
        }
        this->script = "";
        this->message = "";
        // Reset process state
        frameDivider = 32;
        frame = 0;
        buf_idx = 0;
        // Reset block
        *block = ProcessBlock();

        if (script == "")
            return;
        this->script = script;

        // Create script engine from path extension
        std::string extension = system::getExtension(system::getFilename(path));
        // Remove the leading dot if it exists
        if (!extension.empty() && extension[0] == '.') {
            extension = extension.substr(1);
        }
//        scriptEngine = createScriptEngine(extension);
        scriptEngine = new LibPDEngine();
        if (!scriptEngine) {
            message = string::f("No engine for .%s extension", extension.c_str());
            return;
        }
        scriptEngine->module = this;

        // Run script
        if (scriptEngine->run(path, script)) {
            // Error message should have been set by ScriptEngine
            delete scriptEngine;
            scriptEngine = NULL;
            return;
        }
    }

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_object_set_new(rootJ, "path", json_string(path.c_str()));

		std::string script = this->script;
		// If we haven't accepted the security of this script, serialize the security-sandboxed script anyway.
		if (script == "")
			script = unsecureScript;
		json_object_set_new(rootJ, "patch", json_stringn(script.data(), script.size()));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pathJ = json_object_get(rootJ, "path");
		if (pathJ) {
			std::string path = json_string_value(pathJ);
			setPath(path);
		}

		// Only get the script string if the script file wasn't found.
		if (this->path != "" && this->script == "") {
			WARN("Patch file %s not found, using script in patch", this->path.c_str());
			json_t* scriptJ = json_object_get(rootJ, "patch");
			if (scriptJ) {
				std::string script = std::string(json_string_value(scriptJ), json_string_length(scriptJ));
				if (script != "") {
					// Request security warning message
					securityAccepted = false;
					securityRequested = true;
					unsecureScript = script;
				}
			}
		}
	}

	bool doesPathExist() {
		if (path == "")
			return false;
		// Try to open file
		std::ifstream file(path);
		return file.good();
	}

	void loadScriptDialog() {
		std::string dir = asset::plugin(pluginInstance, "patches");
		char* pathC = osdialog_file(OSDIALOG_OPEN, dir.c_str(), NULL, NULL);
		if (!pathC) {
			return;
		}
		std::string path = pathC;
		std::free(pathC);

		setPath(path);
	}

	void reloadScript() {
		loadPath();
	}

	void editScript() {
		std::string editorPath = getEditorPath();
		if (editorPath.empty())
			return;
		if (path.empty())
			return;
		// Launch editor and detach
#if defined ARCH_LIN
		std::string command = editorPath + " \"" + path + "\" &";
		(void) std::system(command.c_str());
#elif defined ARCH_MAC
		std::string command = "open -a " + editorPath + " \"" + path + "\" &";
		(void) std::system(command.c_str());
#elif defined ARCH_WIN
		std::string command = editorPath + " \"" + path + "\"";
		int commandWLen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
		if (commandWLen <= 0)
			return;
		std::wstring commandW(commandWLen, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &commandW[0], commandWLen);
		STARTUPINFOW startupInfo;
		std::memset(&startupInfo, 0, sizeof(startupInfo));
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo;
		// Use the non-const [] accessor for commandW. Since C++11, it is null-terminated.
		if (CreateProcessW(NULL, &commandW[0], NULL, NULL, false, 0, NULL, NULL, &startupInfo, &processInfo)) {
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
		}
#endif
	}

	void setClipboardMessage() {
		glfwSetClipboardString(APP->window->win, message.c_str());
	}

	void appendContextMenu(Menu* menu) {
/*		struct NewScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->newScriptDialog();
			}
		};
		NewScriptItem* newScriptItem = createMenuItem<NewScriptItem>("New patch");
		newScriptItem->module = this;
		menu->addChild(newScriptItem);*/

		struct LoadScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->loadScriptDialog();
			}
		};
		LoadScriptItem* loadScriptItem = createMenuItem<LoadScriptItem>("Load patch");
		loadScriptItem->module = this;
		menu->addChild(loadScriptItem);

		struct ReloadScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->reloadScript();
			}
		};
		ReloadScriptItem* reloadScriptItem = createMenuItem<ReloadScriptItem>("Reload patch");
		reloadScriptItem->module = this;
		menu->addChild(reloadScriptItem);

		struct EditScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->editScript();
			}
		};
		EditScriptItem* editScriptItem = createMenuItem<EditScriptItem>("Edit patch");
		editScriptItem->module = this;

		editScriptItem->disabled = !doesPathExist() || (getEditorPath() == "");
		menu->addChild(editScriptItem);

		menu->addChild(new MenuSeparator);

		struct SetPdEditorItem : MenuItem {
			void onAction(const event::Action& e) override {
				setPdEditorDialog();
			}
		};
		SetPdEditorItem* setPdEditorItem = createMenuItem<SetPdEditorItem>("Set Pure Data application");
		menu->addChild(setPdEditorItem);
	}

	std::string getEditorPath() {
		if (path == "")
			return "";
		// HACK check if extension is .pd
//		if (string::filenameExtension(string::filename(path)) == "pd")
//        if (system::getExtension(system::getFilename(path)) == "pd")
			return settingsPdEditorPath;
	}
    
};

void midiByteHook(int port, int byte) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t b = (uint8_t)byte;
    
    // Status byte
    if (b & 0x80) {
        module->midiRunningStatus = b;
        module->midiDataIndex = 0;
        
        // Determine bytes needed
        uint8_t type = b & 0xF0;
        if (type == 0xC0 || type == 0xD0) {
            module->midiDataBytesNeeded = 1;
        } else if ((type >= 0x80 && type <= 0xE0) || type == 0xF0) {
            module->midiDataBytesNeeded = 2;
        }
        return;
    }
    
    // Data byte
    if (module->midiRunningStatus == 0) return;
    
    module->midiDataBuffer[module->midiDataIndex++] = b;
    
    // Send when we have all data bytes
    if (module->midiDataIndex >= module->midiDataBytesNeeded) {
        midi::Message msg;
        msg.setSize(module->midiDataBytesNeeded + 1);
        msg.bytes[0] = module->midiRunningStatus;
        msg.bytes[1] = module->midiDataBuffer[0];
        if (module->midiDataBytesNeeded > 1) {
            msg.bytes[2] = module->midiDataBuffer[1];
        }
        module->midiOutput.sendMessage(msg);
        module->midiDataIndex = 0;
    }
}

void midiNoteHook(int channel, int pitch, int velocity) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0x90 | (channel & 0x0F);
    
    midi::Message msg;
    msg.setSize(3);
    msg.bytes[0] = status;
    msg.bytes[1] = pitch;
    msg.bytes[2] = velocity;
    module->midiOutput.sendMessage(msg);
}

void midiControlChangeHook(int channel, int controller, int value) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0xB0 | (channel & 0x0F);
    
    midi::Message msg;
    msg.setSize(3);
    msg.bytes[0] = status;
    msg.bytes[1] = controller;
    msg.bytes[2] = value;
    module->midiOutput.sendMessage(msg);
}

void midiProgramChangeHook(int channel, int value) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0xC0 | (channel & 0x0F);
    
    midi::Message msg;
    msg.setSize(2);
    msg.bytes[0] = status;
    msg.bytes[1] = value;
    module->midiOutput.sendMessage(msg);
}

void midiPitchBendHook(int channel, int value) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0xE0 | (channel & 0x0F);
    int bend = value + 8192; // Convert from -8192-8191 to 0-16383
    
    midi::Message msg;
    msg.setSize(3);
    msg.bytes[0] = status;
    msg.bytes[1] = bend & 0x7F;
    msg.bytes[2] = (bend >> 7) & 0x7F;
    module->midiOutput.sendMessage(msg);
}

void midiAfterTouchHook(int channel, int value) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0xD0 | (channel & 0x0F);
    
    midi::Message msg;
    msg.setSize(2);
    msg.bytes[0] = status;
    msg.bytes[1] = value;
    module->midiOutput.sendMessage(msg);
}

void midiPolyAfterTouchHook(int channel, int pitch, int value) {
    LibPDEngine* engine = g_current_engine;
    if (!engine || !engine->module) return;
    
    PureData* module = engine->module;
    uint8_t status = 0xA0 | (channel & 0x0F);
    
    midi::Message msg;
    msg.setSize(3);
    msg.bytes[0] = status;
    msg.bytes[1] = pitch;
    msg.bytes[2] = value;
    module->midiOutput.sendMessage(msg);
}

void LibPDEngine::send_toHost(const char* source, const char* symbol, int argc, t_atom* argv) {
    LibPDEngine* engine = g_current_engine;
    if (!engine) return;
    
    if (strcmp(source, "toHost") != 0) return;
    
    std::string selector = symbol;

// Handle MIDI
    if(selector == "midiinit"){ // MIDI check and initialization to default
        auto driverIds = midi::getDriverIds();
        if(!driverIds.empty()){ // Check available drivers
            fprintf(stderr, "\n");
//            fprintf(stderr, "MIDI DRIVER CHECK\n");
            fprintf(stderr, "Available MIDI Input drivers:\n");
            for(int id : driverIds){
                midi::Driver* driver = midi::getDriver(id);
                if(driver){
                    fprintf(stderr, "  %d: %s\n", id, driver->getName().c_str());
                    // Check Devices
/*                    auto deviceIds = engine->module->midiInput.getDeviceIds();
                    if (deviceIds.empty()) {
                        fprintf(stderr, "No MIDI input devices found\n");
                    }
                    else{
                        fprintf(stderr, "MIDI input devices:\n");
                        for(size_t i = 0; i < deviceIds.size(); i++)
                            fprintf(stderr, "  %zu: %s\n", i, engine->module->midiInput.getDeviceName(deviceIds[i]).c_str());
                    }*/
                }
            }
        }
        else
            fprintf(stderr, "No Available MIDI drivers\n");
        // set Input driver
        engine->module->midiInput.setDriverId(-1);
        auto* driver = engine->module->midiInput.getDriver();
        fprintf(stderr, "\n");
        fprintf(stderr, "SET MIDI IN / MIDI OUT\n");
        if(driver){
            fprintf(stderr, "MIDI Input driver set to: %s\n", driver->getName().c_str());
            // Check Devices
            auto deviceIds = engine->module->midiInput.getDeviceIds();
            if(deviceIds.empty())
                fprintf(stderr, "No MIDI input devices found\n");
            else{
                fprintf(stderr, "MIDI input devices:\n");
                for(size_t i = 0; i < deviceIds.size(); i++)
                    fprintf(stderr, "  %zu: %s\n", i, engine->module->midiInput.getDeviceName(deviceIds[i]).c_str());
            }
        }
        else
            fprintf(stderr, "No MIDI Input driver is set (NULL)\n");
        // set Output driver
        engine->module->midiOutput.setDriverId(-1);
        auto* outDriver = engine->module->midiOutput.getDriver();
        if(outDriver){
            fprintf(stderr, "MIDI Output driver set to: %s\n", outDriver->getName().c_str());
            // Check Devices
            auto outDeviceIds = engine->module->midiOutput.getDeviceIds();
            if(outDeviceIds.empty())
                fprintf(stderr, "No MIDI output devices found\n");
            else{
                fprintf(stderr, "MIDI output devices:\n");
                for(size_t i = 0; i < outDeviceIds.size(); i++)
                    fprintf(stderr, "  %zu: %s\n", i, engine->module->midiOutput.getDeviceName(outDeviceIds[i]).c_str());
            }
        }
        else
            fprintf(stderr, "No MIDI Output driver is set (NULL)\n");
    }

// Set MIDI IN DRIVER
    if(selector == "midiindriver"){
        if(argc == 1 && libpd_is_float(&argv[0])){
            int driverId = (int)libpd_get_float(&argv[0]);
            engine->module->midiInput.setDriverId(driverId);
            fprintf(stderr, "MIDI Input driver set to: %s\n", engine->module->midiInput.getDriver()->getName().c_str());
            // List devices for this driver
            auto deviceIds = engine->module->midiInput.getDeviceIds();
            if(deviceIds.empty())
                fprintf(stderr, "No MIDI input devices found for this driver\n");
            else{
                fprintf(stderr, "MIDI input devices:\n");
                for(size_t i = 0; i < deviceIds.size(); i++){
                    fprintf(stderr, "  %zu: %s\n", i, engine->module->midiInput.getDeviceName(deviceIds[i]).c_str());
                }
            }
        }
        return;
    }
// Set MIDI IN DEVICE
    if(selector == "midiindev"){
        if(argc == 1 && libpd_is_float(&argv[0])){
            int deviceId = (int)libpd_get_float(&argv[0]);
            if(deviceId == -1){
                engine->module->midiInput.setDeviceId(-1);
                fprintf(stderr, "MIDI IN device set to: none\n");
            }
            else{
                auto deviceIds = engine->module->midiInput.getDeviceIds();
                if(deviceId >= 0 && deviceId < (int)deviceIds.size()){
                    engine->module->midiInput.setDeviceId(deviceIds[deviceId]);
                    fprintf(stderr, "MIDI IN device set to: %s\n", engine->module->midiInput.getDeviceName(engine->module->midiInput.getDeviceId()).c_str());
                }
                else
                    fprintf(stderr, "Invalid MIDI IN device ID: %d\n", deviceId);
            }
        }
        return;
    }
    
// Set MIDI output DRIVER
    if(selector == "midioutdriver"){
        if(argc == 1 && libpd_is_float(&argv[0])){
            int driverId = (int)libpd_get_float(&argv[0]);
            engine->module->midiOutput.setDriverId(driverId);
            fprintf(stderr, "MIDI output driver set to: %s\n", engine->module->midiOutput.getDriver()->getName().c_str());
            
            auto deviceIds = engine->module->midiOutput.getDeviceIds();
            if (deviceIds.empty()) {
                fprintf(stderr, "No MIDI output devices found for this driver\n");
            }
            else{
                fprintf(stderr, "MIDI output devices:\n");
                for(size_t i = 0; i < deviceIds.size(); i++){
                    fprintf(stderr, "  %zu: %s\n", i, engine->module->midiOutput.getDeviceName(deviceIds[i]).c_str());
                }
            }
        }
        return;
    }
// Set MIDI output DEVICE
    if(selector == "midioutdev"){
        if(argc == 1 && libpd_is_float(&argv[0])){
            int deviceId = (int)libpd_get_float(&argv[0]);
            if (deviceId == -1) {
                engine->module->midiOutput.setDeviceId(-1);
                fprintf(stderr, "MIDI output device set to: none\n");
            }
            else{
                auto deviceIds = engine->module->midiOutput.getDeviceIds();
                if (deviceId >= 0 && deviceId < (int)deviceIds.size()) {
                    engine->module->midiOutput.setDeviceId(deviceIds[deviceId]);
                    fprintf(stderr, "MIDI output device set to: %s\n", engine->module->midiOutput.getDeviceName(engine->module->midiOutput.getDeviceId()).c_str());
                }
                else
                    fprintf(stderr, "Invalid MIDI output device ID: %d\n", deviceId);
            }
        }
        return;
    }
    
// Handle lights
    int light_idx = -1;
    try {
        light_idx = engine->_light_map.at(selector);
        if (argc == 3) {
            engine->_lights[light_idx][0] = libpd_get_float(&argv[0]);
            engine->_lights[light_idx][1] = libpd_get_float(&argv[1]);
            engine->_lights[light_idx][2] = libpd_get_float(&argv[2]);
        }
        return;
    } catch (...) {}
    
    // Handle switch lights
    int switch_idx = -1;
    try {
        switch_idx = engine->_switchLight_map.at(selector);
        if (argc == 3) {
            engine->_switchLights[switch_idx][0] = libpd_get_float(&argv[0]);
            engine->_switchLights[switch_idx][1] = libpd_get_float(&argv[1]);
            engine->_switchLights[switch_idx][2] = libpd_get_float(&argv[2]);
        }
        return;
    } catch (...) {}
    
    // Handle display
    if (selector == "display") {
        engine->_utility[0] = "display";
        engine->_utility[1] = "";
        for (int i = 0; i < argc; i++) {
            if (i > 0) engine->_utility[1] += " ";
            if (libpd_is_float(&argv[i])) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", libpd_get_float(&argv[i]));
                engine->_utility[1] += buf;
            } else if (libpd_is_symbol(&argv[i])) {
                engine->_utility[1] += libpd_get_symbol(&argv[i]);
            }
        }
        engine->_display_is_valid = true;
    }
}

void ScriptEngine::display(const std::string& message) {
	module->message = message;
}
void ScriptEngine::setFrameDivider(int frameDivider) {
	module->frameDivider = std::max(frameDivider, 1);
}
void ScriptEngine::setBufferSize(int bufferSize) {
	module->block->bufferSize = clamp(bufferSize, 1, MAX_BUFFER_SIZE);
}
ProcessBlock* ScriptEngine::getProcessBlock() {
	return module->block;
}

struct FileChoice : LedDisplayChoice {
	PureData* module;
    void step() override {
        if (module && module->path != ""){
            text = "patch file: ";
            text += system::getFilename(module->path);
        }
        else
            text = "(click to load a file)";
    }
    
	void onAction(const event::Action& e) override {
		Menu* menu = createMenu();
		module->appendContextMenu(menu);
	}
};


struct MessageChoice : LedDisplayChoice {
	PureData* module;
    void step() override {
        if (module && module->message != "") {
            text = module->message;
        } else {
            std::string version = std::to_string(PD_MAJOR_VERSION) + "." +
                                  std::to_string(PD_MINOR_VERSION) + "-" +
                                  std::to_string(PD_BUGFIX_VERSION);
            text = "Running Pd-" + version;
        }
    }
    void draw(const DrawArgs& args) override {
        nvgScissor(args.vg, RECT_ARGS(args.clipBox));
        // Load font - in V2, fonts are loaded from the system
        std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (font) {
            nvgFillColor(args.vg, color);
            nvgFontFaceId(args.vg, font->handle);
            nvgTextLetterSpacing(args.vg, 0.0);
            nvgTextLineHeight(args.vg, 1.08);
            nvgFontSize(args.vg, 12);
            nvgTextBox(args.vg, textOffset.x, textOffset.y, box.size.x - textOffset.x, text.c_str(), NULL);
        }
        nvgResetScissor(args.vg);
    }
	void onAction(const event::Action& e) override {
		Menu* menu = createMenu();

		struct SetClipboardMessageItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->setClipboardMessage();
			}
		};
		SetClipboardMessageItem* item = createMenuItem<SetClipboardMessageItem>("Copy");
		item->module = module;
		menu->addChild(item);
	}
};

struct PureDataDisplay : LedDisplay {
	PureDataDisplay() {
		box.size = mm2px(Vec(69.879, 27.335));
	}
    
	void setModule(PureData* module) {
		FileChoice* fileChoice = new FileChoice;
		fileChoice->box.size.x = box.size.x;
		fileChoice->module = module;
		addChild(fileChoice);

		LedDisplaySeparator* fileSeparator = new LedDisplaySeparator;
		fileSeparator->box.size.x = box.size.x;
		fileSeparator->box.pos = fileChoice->box.getBottomLeft();
		addChild(fileSeparator);

		MessageChoice* messageChoice = new MessageChoice;
		messageChoice->box.pos = fileChoice->box.getBottomLeft();
		messageChoice->box.size.x = box.size.x;
		messageChoice->box.size.y = box.size.y - messageChoice->box.pos.y;
		messageChoice->module = module;
		addChild(messageChoice);
	}
};


struct PureDataWidget : ModuleWidget {
	PureDataWidget(PureData* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Pd-Host.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(8.099, 64.401)), module, PureData::KNOB_PARAMS + 0));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(20.099, 64.401)), module, PureData::KNOB_PARAMS + 1));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(32.099, 64.401)), module, PureData::KNOB_PARAMS + 2));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(44.099, 64.401)), module, PureData::KNOB_PARAMS + 3));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(56.099, 64.401)), module, PureData::KNOB_PARAMS + 4));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(68.099, 64.401)), module, PureData::KNOB_PARAMS + 5));
		addParam(createParamCentered<PB61303>(mm2px(Vec(8.099, 80.151)), module, PureData::SWITCH_PARAMS + 0));
		addParam(createParamCentered<PB61303>(mm2px(Vec(20.099, 80.151)), module, PureData::SWITCH_PARAMS + 1));
		addParam(createParamCentered<PB61303>(mm2px(Vec(32.099, 80.151)), module, PureData::SWITCH_PARAMS + 2));
		addParam(createParamCentered<PB61303>(mm2px(Vec(44.099, 80.151)), module, PureData::SWITCH_PARAMS + 3));
		addParam(createParamCentered<PB61303>(mm2px(Vec(56.099, 80.151)), module, PureData::SWITCH_PARAMS + 4));
		addParam(createParamCentered<PB61303>(mm2px(Vec(68.099, 80.151)), module, PureData::SWITCH_PARAMS + 5));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.099, 97.25)), module, PureData::IN_INPUTS + 0));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.099, 97.25)), module, PureData::IN_INPUTS + 1));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.099, 97.25)), module, PureData::IN_INPUTS + 2));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.099, 97.25)), module, PureData::IN_INPUTS + 3));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(56.099, 97.25)), module, PureData::IN_INPUTS + 4));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(68.099, 97.25)), module, PureData::IN_INPUTS + 5));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.099, 112.25)), module, PureData::OUT_OUTPUTS + 0));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.099, 112.25)), module, PureData::OUT_OUTPUTS + 1));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.099, 112.25)), module, PureData::OUT_OUTPUTS + 2));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.099, 112.25)), module, PureData::OUT_OUTPUTS + 3));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(56.099, 112.25)), module, PureData::OUT_OUTPUTS + 4));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(68.099, 112.25)), module, PureData::OUT_OUTPUTS + 5));

		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(8.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 0));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(20.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 1));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(32.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 2));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(44.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 3));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(56.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 4));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(68.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 5));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(8.099, 80.151)), module, PureData::SWITCH_LIGHTS + 0));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(20.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 1));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(32.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 2));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(44.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 3));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(56.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 4));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(68.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 5));

		PureDataDisplay* display = createWidget<PureDataDisplay>(mm2px(Vec(3.16, 14.837)));
		display->setModule(module);
		addChild(display);
	}

	void appendContextMenu(Menu* menu) override {
		PureData* module = dynamic_cast<PureData*>(this->module);

		menu->addChild(new MenuSeparator);
		module->appendContextMenu(menu);
	}

	void onPathDrop(const event::PathDrop& e) override {
		PureData* module = dynamic_cast<PureData*>(this->module);
		if (!module)
			return;
		if (e.paths.size() < 1)
			return;
		module->setPath(e.paths[0]);
	}

	void step() override {
		PureData* module = dynamic_cast<PureData*>(this->module);
		if (module && module->securityRequested) {
			if (osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK_CANCEL, "VCV PureData is requesting to run a script from a patch or module preset. Running PureData scripts from untrusted sources may compromise your computer and personal information. Proceed and run script?")) {
				module->securityAccepted = true;
			}
			module->securityRequested = false;
		}
		ModuleWidget::step();
	}
};

void init(Plugin* p) {
	pluginInstance = p;

	p->addModel(createModel<PureData, PureDataWidget>("Pd-Host"));
	settingsLoad();
}
