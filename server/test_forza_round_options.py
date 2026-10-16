"""The built-in rounds' car classes (issue #6): the Horizon World takes any class (-1, as
the game's own free roam preset), the Races playlist keeps its 255 for the class vote."""

import unittest
import xml.etree.ElementTree as ET

import forza_live_lsp as lsp


def classes(xml):
    root = ET.fromstring(xml)
    restriction = root.find("Buckets/Bucket/CarRestrictions")
    return (restriction.get("CarClassId"), root.get("MinRecommendedCarClass"),
            root.get("MaxRecommendedCarClass"))


class RoundOptionsTest(unittest.TestCase):
    def test_world_takes_any_class(self):
        xml = lsp.default_round_xml(lsp.WORLD_HOPPER, 0)
        self.assertEqual(classes(xml), ("-1", "-1", "-1"))
        self.assertEqual(ET.fromstring(xml).get("GameTypePreset"), "7")

    def test_races_keep_their_class(self):
        for n in range(4):
            self.assertEqual(classes(lsp.default_round_xml(1, n)), ("255", "255", "255"))


if __name__ == "__main__":
    unittest.main()
