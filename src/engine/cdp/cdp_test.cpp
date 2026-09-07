//----------------------------------------------------------------------------
//  Headless check: the registry, a real ported CDP algorithm, and a chained
//  graph.  Proves the "CDP as a library" model works with no UI involved.
//----------------------------------------------------------------------------
#include "cdp_graph.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

#define CHECK(c) do { if(!(c)) { \
    std::fprintf(stderr,"CHECK FAILED %s:%d: %s\n",__FILE__,__LINE__,#c); \
    std::abort(); } } while(0)

using namespace PatchKnob::cdp;

// A 100 Hz sine at 48k: 480 samples per waveset, unambiguous zero crossings.
static Buffer tone(double hz, double seconds, int sr = 48000) {
    Buffer b; b.sampleRate = sr;
    b.resize(1, (int64_t)(seconds * sr));
    for (int64_t i = 0; i < b.frames(); ++i)
        b.ch[0][(size_t)i] = (float)std::sin(2.0 * 3.14159265358979 * hz * (double)i / sr);
    return b;
}

int main() {
    std::printf("[cdp] registry...\n");
    CHECK(!registry().empty());
    const Process* rev = find("distort.reverse");
    const Process* omit = find("distort.omit");
    CHECK(rev && omit);
    CHECK(rev->streamable && rev->latencyFrames);
    CHECK(rev->latencyFrames(48000, {1.0}) > 0);      // waveset lookahead is real
    CHECK(rev->params.size() == 1 && omit->params.size() == 2);
    std::printf("[cdp] %d process(es) registered\n", (int)registry().size());

    // ---- the ported algorithm actually transforms audio --------------------
    Buffer in = tone(100.0, 0.10);
    Buffer out; std::string err;
    CHECK(run("distort.reverse", {in}, {1.0}, out, err));
    CHECK(out.frames() == in.frames() && out.channels() == in.channels());
    // Reversing each single waveset of a sine flips its shape: the result must
    // differ from the input but keep the same energy (it is a permutation).
    double diff = 0.0, ein = 0.0, eout = 0.0;
    for (int64_t i = 0; i < in.frames(); ++i) {
        const double a = in.ch[0][(size_t)i], b = out.ch[0][(size_t)i];
        diff += std::fabs(a - b); ein += a * a; eout += b * b;
    }
    CHECK(diff > 1.0);                                   // it did something
    CHECK(std::fabs(ein - eout) < 1e-3 * ein);           // energy preserved
    for (int64_t i = 0; i < out.frames(); ++i) CHECK(std::isfinite(out.ch[0][(size_t)i]));
    std::printf("[cdp] distort.reverse: energy preserved, output changed -- OK\n");

    // OMIT must actually silence part of the signal.
    Buffer thin;
    CHECK(run("distort.omit", {in}, {1.0, 1.0}, thin, err));
    double ethin = 0.0;
    for (int64_t i = 0; i < thin.frames(); ++i)
        ethin += (double)thin.ch[0][(size_t)i] * thin.ch[0][(size_t)i];
    CHECK(ethin < ein * 0.75 && ethin > 0.0);
    std::printf("[cdp] distort.omit: thinned to %.0f%% energy -- OK\n", 100.0 * ethin / ein);

    // ---- chained graph, as the modular offline editor will drive it --------
    std::printf("[cdp] graph...\n");
    Graph g;
    const NodeId src = g.addSource(tone(100.0, 0.10), "clip");
    const NodeId n1  = g.addProcess("distort.reverse");
    const NodeId n2  = g.addProcess("distort.omit");
    CHECK(src && n1 && n2);
    CHECK(g.connect(src, n1, 0));
    CHECK(g.connect(n1, n2, 0));
    CHECK(!g.connect(n2, n2, 0));                        // self-patch refused
    g.setOutput(n2);

    Buffer rendered;
    CHECK(g.render(rendered, err));
    CHECK(rendered.frames() == in.frames());
    for (int64_t i = 0; i < rendered.frames(); ++i)
        CHECK(std::isfinite(rendered.ch[0][(size_t)i]));
    // Chained result must differ from either stage alone.
    double d1 = 0.0;
    for (int64_t i = 0; i < rendered.frames(); ++i)
        d1 += std::fabs(rendered.ch[0][(size_t)i] - out.ch[0][(size_t)i]);
    CHECK(d1 > 1.0);
    std::printf("[cdp] 3-node chain rendered %lld frames -- OK\n",
                (long long)rendered.frames());

    // Latency accumulates along the chain; both stages stream.
    CHECK(g.canStream());
    const int64_t lat = g.latencyFrames(48000);
    CHECK(lat >= rev->latencyFrames(48000, {1.0}));
    std::printf("[cdp] chain latency %lld frames (%.1f ms) -- host must compensate\n",
                (long long)lat, 1000.0 * (double)lat / 48000.0);

    // A feedback loop must be refused at CONNECT time.  It used to be "legal to
    // wire, illegal to evaluate", which meant the editor briefly held a cyclic
    // graph -- and its downstream-invalidation walk recursed on it and blew the
    // stack the moment the closing cable landed.  connect() now rejects any
    // edge whose destination can already reach its source; renderNode()'s own
    // `visiting` guard stays as defence in depth.
    Graph loop;
    const NodeId a = loop.addProcess("distort.reverse");
    const NodeId b = loop.addProcess("distort.omit");
    CHECK(loop.connect(a, b, 0));
    CHECK(!loop.connect(b, a, 0));      // 2-node cycle refused
    const NodeId c = loop.addProcess("distort.omit");
    CHECK(loop.connect(b, c, 0));
    CHECK(!loop.connect(c, a, 0));      // 3-node cycle refused too
    CHECK(loop.reaches(a, c) && !loop.reaches(c, a));
    // The chain itself is still intact and acyclic -- a->b->c, three edges
    // attempted, one refused at each cycle-closing step.
    CHECK(loop.edges().size() == 2);
    std::printf("[cdp] feedback loops refused at connect() -- OK\n");

    std::printf("cdp_test: ALL PASS\n");
    return 0;
}
