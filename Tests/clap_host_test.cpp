// clap_host_test.cpp
// A minimal CLAP host: loads the built plugin binary, then checks the
// descriptor, parameters, text conversion, processing, automation events,
// state save/restore, latency and tail.
//
//   clap_host_test <path to the .clap binary>
// On macOS pass the binary inside the bundle:
//   "TB M50 Single Channel Strip.clap/Contents/MacOS/TB M50 Single Channel Strip"

#include <clap/clap.h>

#include "Params.h"
#include "M50.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++failures;
}

// ---- Host ----
static int flushRequests = 0, rescans = 0;
static void hpRescan (const clap_host_t*, clap_param_rescan_flags) { ++rescans; }
static void hpClear (const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void hpRequestFlush (const clap_host_t*) { ++flushRequests; }
static const clap_host_params_t hostParams = { hpRescan, hpClear, hpRequestFlush };

static const void* hGetExt (const clap_host_t*, const char* id)
{
    return ! std::strcmp (id, CLAP_EXT_PARAMS) ? &hostParams : nullptr;
}
static void hNoop (const clap_host_t*) {}
static const clap_host_t host = { CLAP_VERSION_INIT, nullptr, "clap_host_test", "TB", "", "1.0",
                                  hGetExt, hNoop, hNoop, hNoop };

// ---- Event lists ----
struct InEvents
{
    std::vector<clap_event_param_value_t> ev;
    clap_input_events_t list;
    InEvents() { list.ctx = this; list.size = &sSize; list.get = &sGet; }
    static uint32_t sSize (const clap_input_events_t* l) { return (uint32_t) ((InEvents*) l->ctx)->ev.size(); }
    static const clap_event_header_t* sGet (const clap_input_events_t* l, uint32_t i) { return &((InEvents*) l->ctx)->ev[i].header; }
    void add (clap_id id, double v)
    {
        clap_event_param_value_t e;
        std::memset (&e, 0, sizeof (e));
        e.header.size = sizeof (e); e.header.space_id = CLAP_CORE_EVENT_SPACE_ID; e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id; e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1; e.value = v;
        ev.push_back (e);
    }
};

struct OutEvents
{
    int values = 0, begins = 0, ends = 0;
    clap_output_events_t list;
    OutEvents() { list.ctx = this; list.try_push = &sPush; }
    static bool sPush (const clap_output_events_t* l, const clap_event_header_t* h)
    {
        OutEvents* o = (OutEvents*) l->ctx;
        if (h->type == CLAP_EVENT_PARAM_VALUE) ++o->values;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) ++o->begins;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_END) ++o->ends;
        return true;
    }
};

// ---- Streams ----
struct OStream { std::string data; clap_ostream_t s; OStream() { s.ctx = this; s.write = &w; }
    static int64_t w (const clap_ostream_t* st, const void* b, uint64_t n) { ((OStream*) st->ctx)->data.append ((const char*) b, (size_t) n); return (int64_t) n; } };
struct IStream { std::string data; size_t pos = 0; clap_istream_t s; IStream() { s.ctx = this; s.read = &r; }
    static int64_t r (const clap_istream_t* st, void* b, uint64_t n)
    {
        IStream* is = (IStream*) st->ctx;
        // Hand the data over a few bytes at a time to prove partial reads work
        size_t k = std::min ((size_t) n, std::min ((size_t) 37, is->data.size() - is->pos));
        std::memcpy (b, is->data.data() + is->pos, k); is->pos += k; return (int64_t) k;
    } };

static bool finite (const std::vector<float>& x, float lim)
{
    for (size_t i = 0; i < x.size(); ++i) if (! (x[i] == x[i]) || std::fabs (x[i]) > lim) return false;
    return true;
}

// Run audio through the plugin in blocks.
// Mono: one channel in, one out
static void run (const clap_plugin_t* p, std::vector<float>& x, std::vector<float>& y, uint32_t block,
                 InEvents* in = nullptr, OutEvents* out = nullptr)
{
    InEvents emptyIn; OutEvents sink;
    for (size_t pos = 0; pos < x.size(); pos += block)
    {
        const uint32_t n = (uint32_t) std::min ((size_t) block, x.size() - pos);
        float* ins[1] = { &x[pos] };
        float* outs[1] = { &y[pos] };
        clap_audio_buffer_t ib; std::memset (&ib, 0, sizeof (ib)); ib.data32 = ins; ib.channel_count = 1;
        clap_audio_buffer_t ob; std::memset (&ob, 0, sizeof (ob)); ob.data32 = outs; ob.channel_count = 1;
        clap_process_t pr; std::memset (&pr, 0, sizeof (pr));
        pr.steady_time = -1; pr.frames_count = n;
        pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        pr.in_events = (in && pos == 0) ? &in->list : &emptyIn.list;
        pr.out_events = out ? &out->list : &sink.list;
        p->process (p, &pr);
    }
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: %s <plugin binary>\n", argv[0]); return 2; }
    std::printf ("Loading %s\n", argv[1]);
    void* lib = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (! lib) { std::printf ("dlopen failed: %s\n", dlerror()); return 1; }
    const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*) dlsym (lib, "clap_entry");
    check (entry != nullptr, "exports clap_entry");
    if (! entry) return 1;
    check (entry->init (argv[1]), "entry init");
    const clap_plugin_factory_t* f = (const clap_plugin_factory_t*) entry->get_factory (CLAP_PLUGIN_FACTORY_ID);
    check (f && f->get_plugin_count (f) == 1, "one plugin in the factory");
    const clap_plugin_descriptor_t* d = f->get_plugin_descriptor (f, 0);
    std::printf ("  %s by %s, id %s, version %s\n", d->name, d->vendor, d->id, d->version);
    check (! std::strcmp (d->name, "TB M50 Single Channel Strip"), "plugin name");

    const clap_plugin_t* p = f->create_plugin (f, &host, d->id);
    check (p && p->init (p), "create and init");
    if (! p) return 1;

    using namespace tbm50s;
    std::printf ("\nParameters\n");
    const clap_plugin_params_t* pp = (const clap_plugin_params_t*) p->get_extension (p, CLAP_EXT_PARAMS);
    check (pp && pp->count (p) == kNumParams && kNumParams == 11, "11 parameters (10 on the strip + bypass)");
    const char* order[kNumParams] = { "Input", "Trim", "EQ Bypass", "High Freq", "High Gain", "Mid Freq", "Mid Gain",
                                      "Low Freq", "Low Gain", "Fader", "Bypass" };
    bool namesOk = true, defaultsOk = true;
    for (uint32_t i = 0; i < pp->count (p); ++i)
    {
        clap_param_info_t info;
        pp->get_info (p, i, &info);
        double v = -1; pp->get_value (p, info.id, &v);
        char txt[64] = { 0 }; pp->value_to_text (p, info.id, v, txt, sizeof (txt));
        std::printf ("  %-10s %-10s = %-9s (%.3f, %g..%g%s)\n", info.name, info.module, txt, v, info.min_value, info.max_value,
                     (info.flags & CLAP_PARAM_IS_STEPPED) ? ", stepped" : "");
        if (std::strcmp (info.name, order[i]) != 0) namesOk = false;
        if (std::fabs (v - info.default_value) > 1e-6) defaultsOk = false;
    }
    check (namesOk, "parameters top to bottom as on the strip");
    check (defaultsOk, "values start at their defaults");
    double tv = 0;
    check (pp->text_to_value (p, kInput, "D.I.", &tv) && tv == 1.0, "text 'D.I.' -> INPUT D.I.");
    check (pp->text_to_value (p, kTrim, "+5", &tv) && std::fabs (Channel::trimDb (kInLine, tv) - 5.0) < 1e-6, "text '+5' -> LINE TRIM +5 dB");
    check (pp->text_to_value (p, kHiFreq, "6 k", &tv) && std::fabs (Channel::eqHz (kHigh, tv) - 6000.0) < 0.5, "text '6 k' -> HIGH at 6 kHz");
    check (pp->text_to_value (p, kMidGain, "-6", &tv) && std::fabs (Channel::eqDb (tv) + 6.0) < 1e-6, "text '-6' -> MID -6 dB");
    check (pp->text_to_value (p, kFader, "-10", &tv) && std::fabs (Channel::faderDb (tv) + 10.0) < 0.01, "text '-10' -> fader -10 dB");
    check (pp->text_to_value (p, kFader, "-inf", &tv) && tv == 0.0, "text '-inf' -> fader down");
    check (pp->text_to_value (p, kBypass, "On", &tv) && tv == 1.0, "text 'On' -> BYPASS in");
    char txt[32];
    pp->value_to_text (p, kHiFreq, 1.0, txt, sizeof (txt));
    check (! std::strcmp (txt, "15.0 kHz"), "HIGH reads 15.0 kHz at the top");
    pp->value_to_text (p, kFader, 1.0, txt, sizeof (txt));
    check (! std::strcmp (txt, "+8.0 dB"), "fader reads +8.0 dB at 10");

    std::printf ("\nAudio\n");
    const clap_plugin_audio_ports_t* ap = (const clap_plugin_audio_ports_t*) p->get_extension (p, CLAP_EXT_AUDIO_PORTS);
    clap_audio_port_info_t pi, po;
    check (ap && ap->count (p, true) == 1 && ap->count (p, false) == 1 && ap->get (p, 0, true, &pi) && ap->get (p, 0, false, &po)
           && pi.channel_count == 1 && po.channel_count == 1, "one mono input and one mono output");
    const clap_plugin_latency_t* lat = (const clap_plugin_latency_t*) p->get_extension (p, CLAP_EXT_LATENCY);
    check (p->activate (p, 44100.0, 1, 4096), "activate at 44.1 kHz");
    p->start_processing (p);
    check (lat && lat->get (p) == 0, "no latency");

    const int sr = 44100;
    std::vector<float> x (sr), y (sr);
    for (int i = 0; i < sr; ++i) x[i] = (float) (0.2 * std::sin (2.0 * 3.14159265358979 * 196.0 * i / sr));
    {
        Channel* ref = new Channel();
        ref->prepare (sr, 0x1234567u);
        std::vector<float> refOut (sr);
        for (int i = 0; i < sr; ++i) refOut[i] = (float) (ref->processMono (x[i] * M50::voltsPerUnit()) / M50::voltsPerUnit());
        delete ref;
        run (p, x, y, 512);
        double diff = 0, si = 0, so = 0;
        for (int i = 0; i < sr; ++i)
        {
            diff = std::max (diff, (double) std::fabs (y[i] - refOut[i]));
            if (i > sr / 2) { si += (double) x[i] * x[i]; so += (double) y[i] * y[i]; }
        }
        std::printf ("  max diff vs DSP core %.2g, level %+.3f dB\n", diff, 10.0 * std::log10 (so / si));
        check (diff < 1e-6, "matches the DSP core sample for sample (same as the 10.8.5 build)");
        check (std::fabs (10.0 * std::log10 (so / si)) < 0.1, "unity at the defaults");
    }
    {
        std::vector<float> a (x);
        run (p, a, a, 64);
        check (finite (a, 10.0f), "in-place buffers");
    }

    std::printf ("\nFader and BYPASS\n");
    {
        InEvents in; in.add (kFader, 0.5);
        run (p, x, y, 512, &in);
        double si = 0, so = 0;
        for (int i = sr / 2; i < sr; ++i) { si += (double) x[i] * x[i]; so += (double) y[i] * y[i]; }
        std::printf ("  fader at 5: %+.2f dB\n", 10.0 * std::log10 (so / si));
        check (std::fabs (10.0 * std::log10 (so / si) + 10.0) < 0.3, "the channel fader sets the output");
        std::vector<float> a (sr), oa (sr);
        for (int i = 0; i < sr; ++i) a[i] = (float) (0.3 * std::sin (0.031 * i));
        InEvents b; b.add (kFader, kDefaults[kFader]); b.add (kHiGain, 1.0); b.add (kBypass, 1.0);
        run (p, a, oa, 512, &b);
        float d = 0;
        for (int i = sr / 10; i < sr; ++i) d = std::max (d, std::fabs (oa[i] - a[i]));
        std::printf ("  BYPASS: largest difference from the input %.2g\n", d);
        check (d == 0.0f, "BYPASS passes the input untouched");
        InEvents o; o.add (kBypass, 0.0);
        run (p, a, oa, 512, &o);
        d = 0;
        for (int i = sr / 10; i < sr; ++i) d = std::max (d, std::fabs (oa[i] - a[i]));
        check (d > 0.01f, "and out again (the EQ is heard)");
        InEvents back; back.add (kHiGain, 0.5);
        run (p, a, oa, 512, &back);
    }

    std::printf ("\nAutomation\n");
    {
        std::vector<float> c (sr);
        for (int i = 0; i < sr; ++i) c[i] = (float) (0.3 * std::sin (2.0 * 3.14159265358979 * 110.0 * i / sr));
        bool ok = true;
        for (size_t pos = 0; pos < c.size(); pos += 64)
        {
            InEvents e;
            const double t = (double) pos / c.size();
            for (int k = 0; k < kNumParams; ++k)
                if (k != kBypass) e.add ((clap_id) k, k == kInput ? (double) ((int) (pos / 4096) % 3) : 0.5 + 0.5 * std::sin (6.2831853 * (k + 1) * t));
            std::vector<float> a1 (c.begin() + pos, c.begin() + std::min (c.size(), pos + 64));
            std::vector<float> o1 (a1.size());
            run (p, a1, o1, 64, &e);
            if (! finite (o1, 100.0f)) ok = false;
        }
        check (ok, "all parameters automated together");
    }

    std::printf ("\nState\n");
    {
        const clap_plugin_state_t* st = (const clap_plugin_state_t*) p->get_extension (p, CLAP_EXT_STATE);
        InEvents set; set.add (kFader, 0.6); set.add (kHiFreq, 0.2); set.add (kInput, 1.0); set.add (kBypass, 1.0);
        std::vector<float> a0 (512, 0.0f), o0 (512);
        run (p, a0, o0, 512, &set);
        OStream os;
        check (st && st->save (p, &os.s) && os.data.size() > 20, "saves state");
        InEvents in; in.add (kFader, 0.1); in.add (kHiFreq, 0.9); in.add (kInput, 2.0); in.add (kBypass, 0.0);
        run (p, a0, o0, 512, &in);
        IStream is; is.data = os.data;
        rescans = 0;
        check (st->load (p, &is.s), "loads state");
        double g, t, su, by;
        pp->get_value (p, kFader, &g); pp->get_value (p, kHiFreq, &t); pp->get_value (p, kInput, &su); pp->get_value (p, kBypass, &by);
        check (std::fabs (g - 0.6) < 1e-6 && std::fabs (t - 0.2) < 1e-6 && su == 1.0 && by == 1.0, "state restores every value (BYPASS too)");
        check (rescans > 0, "host is told to re-read the values");
        IStream junk; junk.data = "not a state";
        check (! st->load (p, &junk.s), "rejects foreign data");
        InEvents on;
        for (int k = 0; k < kNumParams; ++k) on.add ((clap_id) k, k == kInput ? (double) inputIndex (kDefaults[k]) : (double) kDefaults[k]);
        run (p, a0, o0, 512, &on);
    }

    std::printf ("\nFlush while stopped\n");
    {
        p->stop_processing (p);
        InEvents in; in.add (kFader, 0.25);
        OutEvents out;
        pp->flush (p, &in.list, &out.list);
        double v; pp->get_value (p, kFader, &v);
        check (std::fabs (v - 0.25) < 1e-6, "flush applies host changes without audio running");
        InEvents back; back.add (kFader, kDefaults[kFader]);
        pp->flush (p, &back.list, &out.list);
    }

    std::printf ("\nSample-rate change to 96 kHz\n");
    p->deactivate (p);
    check (p->activate (p, 96000.0, 1, 4096), "re-activate at 96 kHz");
    p->start_processing (p);
    {
        std::vector<float> a (96000), oa (96000);
        for (int i = 0; i < 96000; ++i) a[i] = (float) (0.2 * std::sin (2.0 * 3.14159265358979 * 196.0 * i / 96000));
        run (p, a, oa, 1024);
        check (finite (oa, 10.0f), "runs at 96 kHz");
        std::vector<float> z (96000 * 2, 0.0f), oz (96000 * 2);
        run (p, z, oz, 1024);
        float t = 0; for (size_t i = 96000; i < oz.size(); ++i) t = std::max (t, std::fabs (oz[i]));
        std::printf ("  residual after 1 s of silence: %.2g (the console's hiss)\n", t);
        check (t < 1e-3f, "silence settles to the noise floor");
    }

    p->stop_processing (p);
    p->deactivate (p);
    p->destroy (p);
    entry->deinit();
    check (true, "destroy and deinit");
    dlclose (lib);

    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL CLAP HOST CHECKS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
