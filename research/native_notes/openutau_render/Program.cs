// Offline synthesis harness using the unmodified, pinned OpenUtau core.
// No plugin, playback, or real-time audio paths use this executable.
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using NAudio.Wave;
using OpenUtau.Core;
using OpenUtau.Core.DiffSinger;
using OpenUtau.Core.Render;
using OpenUtau.Core.Ustx;
using OpenUtau.Core.Util;
using Format = OpenUtau.Core.Format.Ustx;

record Note(int position, int duration, int tone, int phrase);
record Job(string id, string singer, string phoneme, double bpm, double seconds,
           Note[] notes, float[] pitch_midi, string output_prefix, int steps, int gender);

static class Program {
    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    static async Task<int> Main(string[] args) {
        if (args.Length != 2) throw new ArgumentException("Usage: OpenUtau.Test SINGERS_ROOT JOBS_JSON");
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        Directory.CreateDirectory(PathManager.Inst.CachePath);
        Directory.CreateDirectory(PathManager.Inst.DataPath);
        Preferences.Default.AdditionalSingerPath = Path.GetFullPath(args[0]);
        Preferences.Default.DiffSingerMergeNearbyPhrases = false;
        Preferences.Default.OnnxRunner = ""; // Functional CPU path; no CoreML compilation per random length.
        SingerManager.Inst.SearchAllSingers();
        Console.WriteLine(JsonSerializer.Serialize(new { singers = SingerManager.Inst.Singers.Values.Select(s => new { s.Id, s.Name }) }));
        if (Directory.Exists(args[1])) {
            var projects = Directory.GetFiles(args[1], "*.ustx", SearchOption.AllDirectories);
            foreach (var path in projects) {
                var project = Format.Load(path);
                if (!project.parts.OfType<UVoicePart>().Any(p => p.notes.Count > 0))
                    throw new InvalidDataException("Empty exported project: " + path);
            }
            Console.WriteLine(JsonSerializer.Serialize(new { validated_projects = projects.Length }));
            return 0;
        }
        var jobs = JsonSerializer.Deserialize<Job[]>(File.ReadAllText(args[1]))
                   ?? throw new InvalidDataException("Missing jobs");
        var failed = 0;
        foreach (var job in jobs) {
            var clock = Stopwatch.StartNew();
            try {
                var metadata = job.output_prefix + ".render.json";
                if (File.Exists(metadata) && File.Exists(job.output_prefix + ".raw.wav")) {
                    using var existing = JsonDocument.Parse(File.ReadAllText(metadata));
                    if (existing.RootElement.GetProperty("id").GetString() == job.id) {
                        Console.WriteLine(JsonSerializer.Serialize(new { job.id, status = "cached" }));
                        continue;
                    }
                    throw new InvalidDataException("Existing render belongs to a different recipe");
                }
                var singer = SingerManager.Inst.Singers.Values.Single(s => s.Id == job.singer);
                singer.EnsureLoaded();
                if (!singer.Loaded) throw new InvalidDataException(string.Join("; ", singer.Errors));
                Preferences.Default.DiffSingerSteps = job.steps;
                Preferences.Default.DiffSingerStepsVariance = 10;
                Preferences.Default.DiffSingerDepth = 1;
                var renderer = new DiffSingerRenderer();
                var project = Format.Create();
                project.tempos[0].bpm = job.bpm;
                project.timeAxis.BuildSegments(project);
                var track = project.tracks[0];
                track.Singer = singer;
                track.RendererSettings.Renderer = renderer;
                var part = new UVoicePart { trackNo = 0, position = 0, name = job.id };
                project.parts.Add(part);
                var created = new List<UNote>();
                foreach (var spec in job.notes) {
                    var n = UNote.Create();
                    n.position = spec.position;
                    n.duration = spec.duration;
                    n.tone = spec.tone;
                    n.lyric = created.Count == 0 || spec.phrase != job.notes[created.Count - 1].phrase
                              ? job.phoneme : "+";
                    n.ExtendedDuration = n.duration;
                    n.vibrato.length = 0;
                    // The entire curve is supplied through PITD. An empty local
                    // curve must not request snapping its nonexistent first point.
                    n.pitch.snapFirst = false;
                    if (created.Count > 0) { n.Prev = created[^1]; created[^1].Next = n; }
                    part.notes.Add(n);
                    created.Add(n);
                }
                foreach (var group in job.notes.Select((n, i) => (n, i)).GroupBy(p => p.n.phrase)) {
                    var indexes = group.Select(p => p.i).ToArray();
                    var first = created[indexes[0]];
                    first.ExtendedDuration = created[indexes[^1]].End - first.position;
                    foreach (var index in indexes.Skip(1)) created[index].Extends = first;
                    part.phonemes.Add(new UPhoneme { position = first.position, phoneme = job.phoneme, Parent = first });
                }
                for (var i = 0; i < part.phonemes.Count; i++) {
                    var phone = part.phonemes[i];
                    if (i > 0) phone.Prev = part.phonemes[i - 1];
                    if (i + 1 < part.phonemes.Count) phone.Next = part.phonemes[i + 1];
                    phone.Validate(new ValidateOptions(), project, track, part, phone.Parent);
                    if (phone.Error) throw new InvalidDataException("Invalid phone: " + phone.ErrorException);
                }
                part.duration = Math.Max(created[^1].End + 480, (int)Math.Ceiling(job.seconds * job.bpm * 8));
                var output = new float[(int)Math.Ceiling(job.seconds * 44100)];
                var layouts = new List<object>();
                var pitchCurve = new UCurve(project.expressions[Format.PITD]);
                foreach (var phrase in RenderPhrase.FromPart(project, track, part)) {
                    for (var i = 0; i < phrase.pitches.Length; i++) {
                        // OpenUtau's pitch grid is five ticks, in MIDI cents.
                        var tick = phrase.position - phrase.leading + i * 5;
                        var seconds = project.timeAxis.TickPosToMsPos(tick) / 1000;
                        var index = Math.Clamp(seconds * 100, 0, job.pitch_midi.Length - 1);
                        var lo = (int)index;
                        var hi = Math.Min(lo + 1, job.pitch_midi.Length - 1);
                        var target = 100 * (job.pitch_midi[lo] + (job.pitch_midi[hi] - job.pitch_midi[lo]) * (float)(index - lo));
                        if (tick >= 0) { pitchCurve.xs.Add(tick); pitchCurve.ys.Add((int)Math.Round(target - phrase.pitches[i])); }
                    }
                }
                // Finalize curves before phrase construction so OpenUtau's cache
                // identity includes the controlled F0 and voice colour.
                part.curves.Add(pitchCurve);
                var genderCurve = new UCurve(project.expressions[Format.GENC]);
                genderCurve.xs.Add(0); genderCurve.ys.Add(job.gender); part.curves.Add(genderCurve);
                foreach (var phrase in RenderPhrase.FromPart(project, track, part)) {
                    using var cancelled = new CancellationTokenSource();
                    var result = await renderer.Render(phrase, new OpenUtau.Core.Render.Progress(phrase.phones.Length), 0, cancelled, false);
                    if (result.samples == null || result.samples.Length == 0) throw new InvalidDataException("Empty render");
                    var sampleOffset = (int)Math.Round((result.positionMs - result.leadingMs) * 44.1);
                    for (var i = Math.Max(0, -sampleOffset); i < result.samples.Length && sampleOffset + i < output.Length; i++)
                        output[sampleOffset + i] += result.samples[i];
                    layouts.Add(new { phrase.positionMs, phrase.durationMs, result.leadingMs,
                                      samples = result.samples.Length, sampleOffset,
                                      phones = phrase.phones.Select(p => new { p.phoneme, p.positionMs, p.durationMs }) });
                }
                if (output.Any(s => !float.IsFinite(s))) throw new InvalidDataException("Nonfinite audio");
                Directory.CreateDirectory(Path.GetDirectoryName(job.output_prefix)!);
                using (var writer = new WaveFileWriter(job.output_prefix + ".raw.wav", WaveFormat.CreateIeeeFloatWaveFormat(44100, 1)))
                    writer.WriteSamples(output, 0, output.Length);
                Format.Save(job.output_prefix + ".ustx", project);
                File.WriteAllText(metadata, JsonSerializer.Serialize(new { job.id, sample_rate = 44100, job.singer,
                    job.steps, job.gender, elapsed_seconds = clock.Elapsed.TotalSeconds, layouts,
                    peak = output.Max(Math.Abs), frames = output.Length }, JsonOptions));
                Console.WriteLine(JsonSerializer.Serialize(new { job.id, status = "rendered", seconds = clock.Elapsed.TotalSeconds }));
            } catch (Exception e) {
                failed++;
                File.WriteAllText(job.output_prefix + ".error.txt", e.ToString());
                Console.Error.WriteLine(JsonSerializer.Serialize(new { job.id, status = "failed", error = e.ToString() }));
            }
        }
        return failed == 0 ? 0 : 1;
    }
}
