import pathlib
import unittest


class RadioSyncTests(unittest.TestCase):
    def test_radio_frame_polling_and_zoom_eligibility(self):
        root = pathlib.Path(__file__).resolve().parents[1] / 'firmware'
        config = (root / 'include' / 'AlbumClockConfig.h').read_text()
        main = (root / 'src' / 'main.cpp').read_text()
        self.assertIn('RADIO_POLL_INTERVAL_MS 1500UL', config)
        self.assertIn('now-lastRadioPoll >= RADIO_POLL_INTERVAL_MS', main)
        self.assertIn('radio||strcmp(event.message,"WiiM artwork")==0', main)
        self.assertIn('faceEnabled&&faceScanAllowed(faceLiveMusic,faceRandomArtwork,musicAllowed,idleAllowed)', main)
        self.assertIn('++nextDisplayGeneration,radio)', main)
        self.assertIn('readServiceJson("/v1/radio/kan88/state", json,3500)', main)

    def test_radio_detection_avoids_redundant_meta_request(self):
        source = (pathlib.Path(__file__).resolve().parents[1] / 'firmware' / 'include' / 'WiimSource.h').read_text()
        poll = source.split('static WiimPlayback pollWiim()',1)[1]
        self.assertLess(poll.index('if (wiimRadioTitle(statusTitle))'),poll.index('getMetaInfo'))
