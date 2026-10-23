import unittest

import numpy as np
import torch

from .pitch_structure import FEATURES, PitchStructureNoteBoundaryNet, structure_features, train_new_inputs_only
from .voicing import VoicingNoteBoundaryNet


class PitchStructureTests(unittest.TestCase):
    def frames(self, pitch):
        return np.column_stack([pitch, np.full(len(pitch), .95), np.ones(len(pitch))])

    def test_warm_start_preserves_v9_predictions(self):
        torch.manual_seed(1701)
        old, new = VoicingNoteBoundaryNet().eval(), PitchStructureNoteBoundaryNet().eval()
        new.initialize_voicing(old.state_dict())
        x = torch.randn(1, FEATURES, 200)
        with torch.inference_mode():
            torch.testing.assert_close(new(x), old(x[:, :138]), atol=1e-6, rtol=1e-5)

    def test_centres_detect_a_step_without_calling_it_modulation(self):
        pitch = np.full(600, 60.); pitch[300:] = 62
        cues = structure_features(self.frames(pitch), len(pitch))
        np.testing.assert_allclose(cues[:4, 300], [2/3, 2/3, 1, 1], atol=1e-6)
        np.testing.assert_allclose(cues[4:6, 300], 0, atol=1e-6)
        np.testing.assert_allclose(cues[8:12, 300], 0, atol=1e-6)

    def test_anchored_training_cannot_modify_existing_v9_weights(self):
        torch.manual_seed(1701)
        model=PitchStructureNoteBoundaryNet()
        model.initialize_voicing(VoicingNoteBoundaryNet().state_dict())
        before={k:v.detach().clone() for k,v in model.state_dict().items()}
        optimizer=torch.optim.AdamW(train_new_inputs_only(model),lr=.001,weight_decay=0)
        model(torch.randn(1,FEATURES,100)).square().mean().backward();optimizer.step()
        for key,value in model.state_dict().items():
            if key=='input_projection.0.weight':
                self.assertTrue(torch.equal(value[:,:138],before[key][:,:138]))
                self.assertFalse(torch.equal(value[:,138:],before[key][:,138:]))
            else:self.assertTrue(torch.equal(value,before[key]))

    def test_modulation_frequency_is_a_soft_feature_including_fast_vibrato(self):
        time = np.arange(600) * .01
        for frequency in (4., 6., 8., 10., 12.):
            with self.subTest(frequency=frequency):
                cues = structure_features(self.frames(60 + .45*np.sin(2*np.pi*frequency*time)), len(time))
                self.assertGreater(float(np.median(cues[10, 100:500])), .9)
                self.assertAlmostEqual(float(np.median(cues[11, 100:500]))*12.5, frequency, delta=.8)
                self.assertLess(float(np.quantile(np.abs(cues[1, 100:500]), .95)), .15)

    def test_smooth_drift_has_trend_without_periodicity(self):
        cues = structure_features(self.frames(60 + np.arange(600)*.005), 600)
        np.testing.assert_allclose(cues[4:6, 100:500], .005*20/3, atol=1e-5)
        self.assertLess(float(cues[10, 100:500].max()), .01)
        self.assertGreater(float(np.min(cues[6:8, 100:500])), .99)

    def test_gaps_are_not_interpolated_and_pitch_is_relative(self):
        frames = self.frames(60 + .5*np.sin(np.arange(600)*.31))
        frames[200:350, 2] = 0
        cues = structure_features(frames, 601)
        shifted = frames.copy(); shifted[:,0] += 12
        np.testing.assert_allclose(cues, structure_features(shifted, 601), atol=1e-5)
        shifted = frames.copy(); shifted[200:350,0] = 110
        np.testing.assert_allclose(cues, structure_features(shifted,601), atol=1e-5)
        np.testing.assert_allclose(cues[:,270], 0, atol=1e-6)
        self.assertTrue(np.isfinite(cues).all())
        self.assertLessEqual(float(np.abs(cues).max()), 1.)

    def test_invalid_grid_and_nonfinite_input_are_rejected(self):
        frames = self.frames(np.full(5,60.))
        for count in (0,3,8):
            with self.assertRaises(ValueError): structure_features(frames,count)
        frames[0,0] = np.nan
        with self.assertRaises(ValueError): structure_features(frames,5)

    def test_articulation_keeps_acoustic_edges_and_silent_rms_finite(self):
        from .prepare_articulation import blend_envelope, offsets, rms_envelope
        notes=[{'start':.2,'end':1.2}];times=np.arange(1501)*.001
        blend=blend_envelope(notes,times)
        self.assertTrue(np.all(blend[times<.215]==0))
        self.assertTrue(np.all(blend[times>1.185]==0))
        self.assertEqual(float(blend[500]),1.)
        for profile in ('vibrato','edge-bend'):
            shift,_=offsets(notes,times,profile,17)
            self.assertTrue(np.all(shift[times<.2]==0))
            self.assertTrue(np.all(shift[times>=1.2]==0))
        wave=np.zeros(10000);wave[2000:3000]=np.sin(np.arange(1000)*.2)*.1
        self.assertTrue(np.isfinite(rms_envelope(wave,16000)).all())


if __name__ == "__main__": unittest.main()
