import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "src"
LAYERS = ("bsp", "driver", "service", "app")


class LayerDependencyTests(unittest.TestCase):
    def test_local_headers_only_depend_on_same_or_lower_layer(self):
        for layer in LAYERS:
            self.assertTrue((ROOT / layer).is_dir(), f"Missing layer: {layer}")
        headers = {
            path.name: (rank, path)
            for rank, layer in enumerate(LAYERS)
            for path in (ROOT / layer).rglob("*.h")
        }
        for rank, layer in enumerate(LAYERS):
            for source in (ROOT / layer).rglob("*"):
                if source.suffix not in (".c", ".h"):
                    continue
                for header in re.findall(r'^#include "([^"]+)"', source.read_text(encoding="utf-8"), re.MULTILINE):
                    if header in headers:
                        target_rank, target = headers[header]
                        with self.subTest(source=str(source.relative_to(ROOT)), header=header):
                            self.assertLessEqual(
                                target_rank, rank,
                                f"{source.relative_to(ROOT)} depends on upper layer {target.relative_to(ROOT)}")


if __name__ == "__main__":
    unittest.main()
