import json
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import os

QMI_TOOL = "/data/local/tmp/qmi_tool"

def run_qmi(command):
    try:
        # Run via su -c
        result = subprocess.run(
            ["su", "-c", f"{QMI_TOOL} {command}"],
            capture_output=True,
            text=True,
            timeout=10
        )
        # Parse output (which might be multiple JSON lines)
        lines = result.stdout.strip().split('\n')
        parsed_data = []
        for line in lines:
            if line.strip().startswith('{'):
                try:
                    parsed_data.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        
        # Combine into a single dict for convenience
        combined = {"raw_lines": parsed_data, "error": result.stderr}
        for item in parsed_data:
            combined.update(item)
            
        return combined
    except Exception as e:
        return {"error": str(e)}

class BandLockHandler(BaseHTTPRequestHandler):
    def _send_cors_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header("Access-Control-Allow-Headers", "X-Requested-With")

    def do_OPTIONS(self):
        self.send_response(200, "ok")
        self._send_cors_headers()
        self.end_headers()

    def do_GET(self):
        parsed_path = urlparse(self.path)
        path = parsed_path.path
        
        if path == '/':
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.end_headers()
            try:
                with open('index.html', 'rb') as f:
                    self.wfile.write(f.read())
            except FileNotFoundError:
                self.wfile.write(b"index.html not found")
            return
            
        elif path == '/api/cell_info':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self._send_cors_headers()
            self.end_headers()
            
            # 1. Get PCI and EARFCN directly from QMI (Bypasses Android Privacy Masking)
            qmi_data = run_qmi("cell_info")
            pci, earfcn = "--", "--"
            neighbors = []
            
            if "cells" in qmi_data and len(qmi_data["cells"]) > 0:
                for c in qmi_data["cells"]:
                    if c.get("type") == "serving":
                        pci = str(c.get("pci", "--"))
                        earfcn = str(c.get("earfcn", "--"))
                    elif c.get("type") == "neighbor":
                        # Convert rsrp -1044 -> -104
                        n_rsrp = int(c.get("rsrp", 0)) // 10 if c.get("rsrp") else "--"
                        neighbors.append({
                            "pci": c.get("pci", "--"),
                            "earfcn": c.get("earfcn", "--"),
                            "rsrp": n_rsrp
                        })
            
            # 2. Get Signal, Band, and CA status from Android API (Dumpsys)
            dumpsys = subprocess.run(["su", "-c", "dumpsys telephony.registry"], capture_output=True, text=True).stdout
            
            band, rsrp, rsrq = "--", "--", "--"
            ca_active = False
            
            import re
            
            # Find Channel Number (EARFCN) as fallback
            match_ch = re.search(r'mChannelNumber=(\d+)', dumpsys)
            if match_ch and earfcn == "--":
                earfcn = match_ch.group(1)
                
            # Find Band
            match_band = re.search(r'mBands=\[([\d, ]+)\]', dumpsys)
            if match_band:
                band = match_band.group(1)
                
            # Find Signal Strength
            match_sig = re.search(r'mLte=CellSignalStrengthLte:.*?rsrp=(-?\d+).*?rsrq=(-?\d+).*?', dumpsys)
            if match_sig:
                rsrp = match_sig.group(1)
                rsrq = match_sig.group(2)
                if rsrp == '2147483647': rsrp = '--'
                if rsrq == '2147483647': rsrq = '--'
                
            # Find CA Status
            if 'isUsingCarrierAggregation=true' in dumpsys:
                ca_active = True
                
            data = {
                "pci": pci,
                "earfcn": earfcn,
                "band": band,
                "rsrp": rsrp,
                "rsrq": rsrq,
                "ca_active": ca_active,
                "neighbors": neighbors
            }
            
            self.wfile.write(json.dumps(data).encode())
            return
            
        elif path == '/api/band_lock':
            query = parse_qs(parsed_path.query)
            band_mask = query.get('mask', [''])[0]
            
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self._send_cors_headers()
            self.end_headers()
            
            if not band_mask:
                self.wfile.write(json.dumps({"error": "Missing mask parameter"}).encode())
                return
                
            data = run_qmi(f"band_lock {band_mask}")
            self.wfile.write(json.dumps(data).encode())
            return
            
        elif path == '/api/unlock':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self._send_cors_headers()
            self.end_headers()
            
            data = run_qmi("unlock")
            self.wfile.write(json.dumps(data).encode())
            return
            
        elif path == '/api/get_pref':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self._send_cors_headers()
            self.end_headers()
            
            data = run_qmi("get_pref")
            self.wfile.write(json.dumps(data).encode())
            return
            
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not found")

def run(server_class=HTTPServer, handler_class=BandLockHandler, port=8080):
    server_address = ('', port)
    httpd = server_class(server_address, handler_class)
    print(f"Starting BandLock Pro Server on http://0.0.0.0:{port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    httpd.server_close()
    print("Server stopped.")

if __name__ == '__main__':
    run()
