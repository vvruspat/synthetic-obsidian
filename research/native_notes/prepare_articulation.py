"""Label-preserving vibrato/edge-bend views of original training clips only."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

import librosa
import numpy as np
import pyworld
from scipy.ndimage import uniform_filter1d
import soundfile as sf

from .augmentation import read_prepared, derivative_record
from .train import digest


def blend_envelope(notes, times):
    blend = np.zeros(len(times))
    for note in notes:
        start, end = note['start'], note['end']
        if end-start < .3: continue
        left, right = np.searchsorted(times,[start,end])
        local = times[left:right]-start
        blend[left:right] = np.clip((np.minimum(local,end-start-local)-.015)/.015,0,1)
    return blend


def rms_envelope(audio, rate):
    # Running-sum roundoff can make silent windows a few ulps negative.
    return np.sqrt(np.maximum(0,uniform_filter1d(audio**2,size=max(1,round(rate*.02)))))


def offsets(notes, times, profile, seed):
    rng = np.random.default_rng(seed)
    shift, blend = np.zeros(len(times)), blend_envelope(notes,times)
    for note in notes:
        start, end = note["start"], note["end"]
        if end - start < .3: continue
        left,right = np.searchsorted(times,[start,end]);ids = np.arange(left,right)
        local = times[ids] - start; duration = end - start
        if profile == "vibrato":
            frequency, extent, phase = rng.uniform(3.5,10.5), rng.uniform(.2,.5), rng.uniform(0,2*np.pi)
            fade = np.clip(np.minimum(local, duration-local) / .06, 0, 1)
            shift[ids] = extent * np.sin(2*np.pi*frequency*local + phase) * fade
        elif profile == "edge-bend":
            width = min(.18, duration*.25)
            attack, tail = rng.uniform(.6,1.2,2) * rng.choice([-1,1],2)
            shift[ids] = attack*np.maximum(0,1-local/width)**2 + tail*np.maximum(0,1-(duration-local)/width)**2
        else: raise ValueError("Unknown articulation profile")
    return shift, blend


def render(parent, cache, directory, implementation):
    directory.mkdir(parents=True, exist_ok=True)
    source, rate = sf.read(parent["audio"],dtype="float32",always_2d=True)
    channel = int(np.argmax(np.mean(source.astype(np.float64)**2,axis=0)))
    original = source[:,channel].astype(np.float64)
    wave = librosa.resample(original,orig_sr=rate,target_sr=16000)
    cached = np.load(cache)
    times = np.arange(len(wave)//80+1)*.005
    f0 = np.interp(times,cached["times"],cached["f0"])
    voiced = np.interp(times,cached["times"],(cached["f0"]>50).astype(float)) >= .5
    f0 = np.where(voiced,np.maximum(f0,50),0).astype(np.float64)
    spectrum = pyworld.cheaptrick(wave,f0,times,16000,f0_floor=50.)
    aperiodicity = pyworld.d4c(wave,f0,times,16000,fft_size=(spectrum.shape[1]-1)*2)
    sample_times = np.arange(len(original))/rate
    envelope = rms_envelope(original,rate)
    blend_samples = blend_envelope(parent['notes'],sample_times)
    rows = []
    for profile in ("vibrato","edge-bend"):
        output = directory/profile;output.mkdir(exist_ok=True);record_file = output/'record.json'
        seed = int(hashlib.sha256((parent['id']+profile+'1802').encode()).hexdigest()[:8],16)
        if record_file.exists():
            record = json.loads(record_file.read_text())
            if (record['augmentation']['implementation_sha256'] != implementation
                    or digest(Path(record['audio'])) != record['audio_sha256']
                    or digest(Path(record['annotation'])) != record['annotation_sha256']):
                raise ValueError("Completed articulation input changed")
            rows.append(record);continue
        shift, _ = offsets(parent['notes'],times,profile,seed)
        modified = f0 * 2**(shift/12)
        rendered = pyworld.synthesize(modified,spectrum,aperiodicity,16000,frame_period=5.)
        rendered = librosa.resample(rendered,orig_sr=16000,target_sr=rate)
        rendered = np.pad(rendered[:len(original)],(0,max(0,len(original)-len(rendered))))
        rendered_envelope = rms_envelope(rendered,rate)
        gain = np.clip(envelope/np.maximum(rendered_envelope,1e-6),.5,2.)
        mask = blend_samples * np.interp(sample_times,times,voiced.astype(float))
        mixed = original*(1-mask) + rendered*gain*mask
        if not np.isfinite(mixed).all(): raise ValueError("Nonfinite WORLD output")
        output_gain = min(1.,.98/max(float(np.abs(mixed).max()),1e-8));mixed *= output_gain
        step = max(1,round(rate*.01))
        after = rms_envelope(mixed,rate)[::step]
        before = envelope[::step]
        correlation = float(np.corrcoef(before,after)[0,1])
        lags = range(-5,6)
        def corr(lag):
            a,b = (before[-lag:],after[:len(after)+lag]) if lag<0 else (before[:len(before)-lag],after[lag:]) if lag>0 else (before,after)
            return float(np.corrcoef(a,b)[0,1])
        lag = max(lags,key=corr)
        quality = {'envelope_correlation':correlation,'envelope_lag_seconds':lag*.01,
                   'accepted':correlation>=.94 and abs(lag)<=1,'policy':'unchanged_sample_grid_and_acoustic_edge_bypass_not_human_review'}
        if not quality['accepted']:
            (output/'rejected.json').write_text(json.dumps(quality,indent=2));print('rejected',parent['id'][:12],profile,quality,flush=True);continue
        path = output/'clip.wav';sf.write(path,np.repeat(mixed[:,None],source.shape[1],axis=1),rate,subtype='PCM_24')
        record = derivative_record(parent,path,0.,'world_label_preserving_'+profile,quality)
        record['sample_rate']=rate;record['ignore_intervals']=[]
        record['augmentation'].update(profile=profile,seed=seed,implementation_sha256=implementation,
            pitch_cache_sha256=digest(cache),world_version=pyworld.__version__,render_gain=output_gain)
        annotation = json.loads(Path(record['annotation']).read_text());annotation['augmentation']=record['augmentation']
        Path(record['annotation']).write_text(json.dumps(annotation,indent=2));record['annotation_sha256']=digest(Path(record['annotation']))
        record_file.write_text(json.dumps(record,indent=2));rows.append(record)
        print('accepted',parent['id'][:12],profile,round(correlation,4),flush=True)
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--clips',type=int,default=20)
    args=p.parse_args();args.base=args.base.resolve();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    records=read_prepared(args.base)
    eligible=[r for r in records if r['split']=='train' and 'augmentation' not in r
              and sum(n['end']-n['start']>=.6 for n in r['notes'])>=2
              and (args.base.parent/'cache'/f"source-{r['id'][:24]}.npz").exists()]
    rng=np.random.default_rng(1802);indices=rng.permutation(len(eligible))[:args.clips]
    implementation=digest(Path(__file__))
    config={'base_sha256':digest(args.base),'implementation_sha256':implementation,'parents':[eligible[i]['id'] for i in indices],
            'profiles':['vibrato','edge-bend'],'world_version':pyworld.__version__,'seed':1802}
    config_path=args.output/'build-config.json'
    if config_path.exists() and json.loads(config_path.read_text())!=config:raise ValueError('Frozen augmentation configuration changed')
    config_path.write_text(json.dumps(config,indent=2))
    derivatives=[]
    for index in indices:
        parent=eligible[index];derivatives.extend(render(parent,args.base.parent/'cache'/f"source-{parent['id'][:24]}.npz",
                                                       args.output/'articulation'/parent['id'][:24],implementation))
    result=json.loads(args.base.read_text());result['base_manifest']=str((args.base.parent/result['base_manifest']).resolve())
    for record in result['records']:
        for key in ('audio','annotation'):record[key]=str((args.base.parent/record[key]).resolve())
        if 'alignment' in record.get('augmentation',{}):record['augmentation']['alignment']=str((args.base.parent/record['augmentation']['alignment']).resolve())
    result['records'].extend(derivatives)
    result['base_manifest']=os.path.relpath(result['base_manifest'],args.output)
    for record in result['records']:
        for key in ('audio','annotation'):record[key]=os.path.relpath(record[key],args.output)
        if 'alignment' in record.get('augmentation',{}):record['augmentation']['alignment']=os.path.relpath(record['augmentation']['alignment'],args.output)
    result['articulation_config']=config
    target=args.output/'prepared.json';target.write_text(json.dumps(result,indent=2,ensure_ascii=False,allow_nan=False))
    checked=read_prepared(target)
    print(json.dumps({'records':len(checked),'articulation_clips':len(derivatives),
                      'articulation_seconds':sum(r['seconds'] for r in derivatives),'manifest_sha256':digest(target)}),flush=True)


if __name__=='__main__':main()
