#ifndef PATCHKNOB_ENGINE_RACK_FUNDAMENTAL_BRIDGE_H
#define PATCHKNOB_ENGINE_RACK_FUNDAMENTAL_BRIDGE_H

#include <memory>

namespace rack { namespace engine { struct Module; } }

namespace rackx { namespace fundamental {
std::unique_ptr<rack::engine::Module> makeVCO();
std::unique_ptr<rack::engine::Module> makeVCF();
std::unique_ptr<rack::engine::Module> makeVCA1();
std::unique_ptr<rack::engine::Module> makeADSR();
std::unique_ptr<rack::engine::Module> makeLFO();
std::unique_ptr<rack::engine::Module> makeMixer();
std::unique_ptr<rack::engine::Module> make8vert();
std::unique_ptr<rack::engine::Module> makeMerge();
std::unique_ptr<rack::engine::Module> makeMidSide();
std::unique_ptr<rack::engine::Module> makeOctave();
std::unique_ptr<rack::engine::Module> makeSplit();
std::unique_ptr<rack::engine::Module> makeSum();
std::unique_ptr<rack::engine::Module> makeVCA2();
std::unique_ptr<rack::engine::Module> makeVCMixer();
std::unique_ptr<rack::engine::Module> makeMutes();
std::unique_ptr<rack::engine::Module> makePulses();
std::unique_ptr<rack::engine::Module> makeRandom();
std::unique_ptr<rack::engine::Module> makeSEQ3();
std::unique_ptr<rack::engine::Module> makeSequentialSwitch1();
std::unique_ptr<rack::engine::Module> makeSequentialSwitch2();
std::unique_ptr<rack::engine::Module> makeStable16();
} }

#endif
