#!/usr/bin/python3

import unittest
import sys
import time

sys.path.append('../util')
import iwd
from iwd import IWD
from iwd import NetworkType
import testutil

from hostapd import HostapdCLI

class Test(unittest.TestCase):

    def validate_connection(self, wd):
        devices = wd.list_devices(1)
        self.assertIsNotNone(devices)
        device = devices[0]

        ordered_network = device.get_ordered_network('ssidEAP-PEAPv0-ISK')

        self.assertEqual(ordered_network.type, NetworkType.eap)

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        ordered_network.network_object.connect()

        condition = 'obj.state == DeviceState.connected'
        wd.wait_for_object_condition(device, condition)

        device.disconnect()

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)


    def test_connection_success(self):
        wd = IWD(True)

        self.validate_connection(wd)

    @classmethod
    def setUpClass(cls):
        IWD.copy_to_storage('ssidEAP-PEAPv0-ISK.8021x')

    @classmethod
    def tearDownClass(cls):
        IWD.clear_storage()

if __name__ == '__main__':
    unittest.main(exit=True)
