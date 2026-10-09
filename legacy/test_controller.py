"""M0 tests for controller.py: P2 sampler fallbacks and the unknown-reading policy.
Run: python3 -m unittest test_controller.py"""
import builtins, subprocess, unittest
from unittest import mock
import controller

VM_STAT = """Mach Virtual Memory Statistics: (page size of 4096 bytes)
Pages free:                              262144.
Pages active:                           1000000.
Pages inactive:                          262144.
Pages speculative:                        65536.
"""
NETSTAT = """Name  Mtu   Network       Address            Ipkts Ierrs     Ibytes    Opkts Oerrs     Obytes  Coll
lo0   16384 <Link#1>                        100     0      5000      100     0      5000     0
en0   1500  <Link#4>      00:11:22:33:44:55  900     0   1000000      800     0     2000000     0
en0   1500  192.168.1     192.168.1.63       900     -   1000000      800     -     2000000     -
en1   1500  <Link#5>      00:11:22:33:44:66   10     0       500       10     0       700     0
"""

def no_proc(real_open):
    def fake(path, *a, **k):
        if str(path).startswith("/proc/"):
            raise OSError("no /proc here")
        return real_open(path, *a, **k)
    return fake

def run_stub(outputs):
    def fake(cmd, *a, **k):
        return subprocess.CompletedProcess(cmd, 0, stdout=outputs.get(cmd[0], ""), stderr="")
    return fake

class PolicyTests(unittest.TestCase):
    def sample(self, **kw):
        s = {"cores": 12, "load1": 1.0, "ram_avail_gb": 32.0, "net_rx_mbps": 0.0,
             "net_tx_mbps": 0.0, "link_mbps": None, "gpu_pct": None}
        s.update(kw)
        return s

    def test_unknown_load_holds(self):
        self.assertEqual(controller.compute_target(self.sample(load1=None), 8), 8)

    def test_unknown_ram_is_not_low_ram(self):
        self.assertEqual(controller.compute_target(self.sample(load1=None, ram_avail_gb=None), 8), 8)

    def test_known_idle_load_still_grows(self):
        self.assertEqual(controller.compute_target(self.sample(load1=0.5), 8), 10)

    def test_unknown_network_skips_nic_cap(self):
        s = self.sample(load1=None, link_mbps=1000, net_rx_mbps=None, net_tx_mbps=None)
        self.assertEqual(controller.compute_target(s, 8), 8)

class MacSamplerTests(unittest.TestCase):
    def setUp(self):
        self.sampler = controller.Sampler()
        self.sampler.os = "Darwin"
        self.patches = [mock.patch.object(builtins, "open", no_proc(builtins.open)),
                        mock.patch.object(controller, "psutil", None),
                        mock.patch.object(controller.subprocess, "run",
                                          run_stub({"vm_stat": VM_STAT, "netstat": NETSTAT,
                                                    "ps": "50.0\n25.0\n"}))]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()

    def test_ram_from_vm_stat(self):
        # free + inactive + speculative = 589824 pages of 4096 bytes = 2.25 GB
        self.assertAlmostEqual(self.sampler._ram_avail_gb(), 2.25, places=2)

    def test_net_counters_from_netstat_skip_loopback_and_duplicates(self):
        self.assertEqual(self.sampler._net_counters(), (1000500, 2000700))

    def test_load_uses_getloadavg(self):
        with mock.patch.object(controller.os, "getloadavg", return_value=(2.5, 2.0, 1.5)):
            self.assertEqual(self.sampler._load(), (2.5, 2.0))

    def test_cpu_from_ps_scaled_by_cores(self):
        self.sampler.cores = 12
        self.assertAlmostEqual(self.sampler._cpu_busy(), 75.0 / 12, places=3)

class UnknownPlatformTests(unittest.TestCase):
    def test_everything_unknown_is_none_not_fake_values(self):
        s = controller.Sampler()
        s.os = "Plan9"
        with mock.patch.object(builtins, "open", no_proc(builtins.open)), \
             mock.patch.object(controller, "psutil", None), \
             mock.patch.object(controller.os, "getloadavg", side_effect=OSError):
            self.assertEqual(s._load(), (None, None))
            self.assertIsNone(s._ram_avail_gb())
            self.assertIsNone(s._cpu_busy())
            self.assertEqual(s._net_mbps(), (None, None))

if __name__ == "__main__":
    unittest.main()
