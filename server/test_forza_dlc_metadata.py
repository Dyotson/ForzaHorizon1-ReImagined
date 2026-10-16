"""DLCMetadata.GetStringTableInfo (#79): the server has no DLC string table and says so with
Id -1, the game's "none" (with Id 0 and no Path the game saved a file without a name to its
marketplace save, that failed, and Horizon Rally said "MARKETPLACE ERROR").

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import unittest

import forza_live_lsp as lsp
import forza_xtsw as x
import forza_xtsw_all as A


class StringTableInfo(unittest.TestCase):
    def test_none(self):
        headers = {"x-classname": "Forza.WebServices.DLCMetadata", "x-method": "GetStringTableInfo",
                   "x-userid": "0009000000000A01"}
        reply = lsp.service_call(headers, bytes(x.encode(A.ForzaDLCMetadataGetStringTableInfo_Params,
                                                         {"language": "EN"})))
        self.assertEqual(reply[:4], bytes(4))  # success
        info = x.decode(A.ForzaDLCMetadataGetStringTableInfo_Results, reply, 12)[0]["stringTableInfo"]
        self.assertEqual(info["base"]["Id"], -1)
        self.assertEqual(info["base"]["Path"], b"")
        self.assertEqual(info["base"]["Size"], 0)


if __name__ == "__main__":
    unittest.main()
