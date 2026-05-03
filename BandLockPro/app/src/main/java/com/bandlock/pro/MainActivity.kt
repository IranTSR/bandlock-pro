package com.bandlock.pro

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import com.bandlock.pro.databinding.ActivityMainBinding
import com.topjohnwu.superuser.Shell
import org.json.JSONObject
import java.io.File

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var qmiToolPath: String = ""

    // All supported LTE bands bitmask (from get_pref unlock value)
    private val ALL_BANDS_MASK = 0x0011e7ffffdf3fffUL

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Initialize libsu
        Shell.getShell { /* Request root access */ }

        setupBinary()
        setupButtons()
        setupRefresh()
        startMonitoring()
    }

    private fun setupBinary() {
        val libDir = applicationInfo.nativeLibraryDir
        val binaryName = "libqmi_tool.so"
        val ndkBinary = File(libDir, binaryName)
        val internalBinary = File(filesDir, "qmi_tool_bin")

        val logBuilder = StringBuilder()
        logBuilder.append("Setting up BandLock Engine...\n")

        if (ndkBinary.exists()) {
            try {
                // Copy to internal files dir for better execution reliability
                ndkBinary.inputStream().use { input ->
                    internalBinary.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
                qmiToolPath = internalBinary.absolutePath

                // Set executable permission via Root
                Shell.cmd("chmod 755 $qmiToolPath").exec()
                logBuilder.append("✅ Engine Ready\n")
            } catch (e: Exception) {
                logBuilder.append("❌ Copy Failed: ${e.message}\n")
                qmiToolPath = ndkBinary.absolutePath
                logBuilder.append("⚠️ Using Fallback Path\n")
            }
        } else {
            logBuilder.append("❌ Native Library Missing! Please Reinstall.\n")
        }

        binding.tvLogs.text = logBuilder.toString()
    }

    private fun setupButtons() {
        // Single bands
        binding.btnLock1.setOnClickListener { lockBand("0x1", "B1") }
        binding.btnLock3.setOnClickListener { lockBand("0x4", "B3") }
        binding.btnLock7.setOnClickListener { lockBand("0x40", "B7") }
        binding.btnLock8.setOnClickListener { lockBand("0x80", "B8") }
        binding.btnLock28.setOnClickListener { lockBand("0x8000000", "B28") }
        binding.btnLock40.setOnClickListener { lockBand("0x8000000000", "B40") }

        // Combo CA bands
        binding.btnLockB1B3.setOnClickListener { lockBand("0x5", "B1+B3") }
        binding.btnLockB3B7.setOnClickListener { lockBand("0x44", "B3+B7") }
        binding.btnLockB3B40.setOnClickListener { lockBand("0x8000000004", "B3+B40") }
        binding.btnLockB1B3B7.setOnClickListener { lockBand("0x45", "B1+B3+B7") }
        binding.btnLockB1B3B28.setOnClickListener { lockBand("0x8000005", "B1+B3+B28") }

        // 5G NR single bands
        binding.btnLockN28.setOnClickListener { lockNrBands("n28", null, "n28") }
        binding.btnLockN41.setOnClickListener { lockNrBands("n41", null, "n41") }
        binding.btnLockN78.setOnClickListener { lockNrBands("n78", null, "n78") }

        // NR Carrier Aggregation combos
        binding.btnLockN28N78.setOnClickListener { lockNrBands("n28+n78", null, "n28+n78") }
        binding.btnLockN41N78.setOnClickListener { lockNrBands("n41+n78", null, "n41+n78") }
        binding.btnLockN28N41N78.setOnClickListener { lockNrBands("n28+n41+n78", null, "n28+n41+n78") }

        // LTE + NR combos
        binding.btnLockB3N78.setOnClickListener { lockNrBands("n78", "0x4", "B3+n78") }
        binding.btnLockB3N28N78.setOnClickListener { lockNrBands("n28+n78", "0x4", "B3+n28+n78") }
        binding.btnLockB1B3B28N78.setOnClickListener { lockNrBands("n28+n78", "0x8000005", "B1+B3+B28+n28+n78") }

        binding.btnUnlock.setOnClickListener { unlockAll() }
        binding.btnModemInfo.setOnClickListener { fetchModemInfo() }
        binding.btnListMbns.setOnClickListener { fetchMbnList() }
    }

    private fun setupRefresh() {
        binding.swipeRefresh.setOnRefreshListener {
            updateSignalInfo()
            updateLockState()
            handler.postDelayed({ binding.swipeRefresh.isRefreshing = false }, 2000)
        }
    }

    private fun lockBand(mask: String, label: String) {
        if (qmiToolPath.isEmpty()) {
            binding.tvLogs.text = "Error: Engine not found"
            return
        }
        binding.tvLogs.text = "Locking to $label..."
        Shell.cmd("$qmiToolPath band_lock $mask").submit { result ->
            val out = result.out.joinToString("\n")
            val err = result.err.joinToString("\n")
            if (result.isSuccess && out.contains("\"OK\"")) {
                binding.tvLogs.text = "✅ Locked to $label"
                binding.tvLockState.text = "🔒 LOCKED: $label"
                binding.tvLockState.setTextColor(0xFFFBBF24.toInt()) // amber
            } else {
                binding.tvLogs.text = "❌ Lock failed\nOUT: $out\nERR: $err"
            }
            // Refresh signal info after lock
            handler.postDelayed({ updateSignalInfo() }, 1500)
        }
    }

    private fun unlockAll() {
        if (qmiToolPath.isEmpty()) return
        binding.tvLogs.text = "Unlocking all bands..."
        Shell.cmd("$qmiToolPath unlock").submit { result ->
            val out = result.out.joinToString("\n")
            val err = result.err.joinToString("\n")
            if (result.isSuccess && out.contains("\"OK\"")) {
                binding.tvLogs.text = "✅ All bands unlocked"
                binding.tvLockState.text = "🔓 UNLOCKED (All Bands)"
                binding.tvLockState.setTextColor(0xFF10B981.toInt()) // green
            } else {
                binding.tvLogs.text = "❌ Unlock failed\nOUT: $out\nERR: $err"
            }
            handler.postDelayed({ updateSignalInfo() }, 1500)
        }
    }

    private fun fetchModemInfo() {
        if (qmiToolPath.isEmpty()) return
        binding.tvLogs.text = "🔍 Fetching Modem Diagnostics..."
        Shell.cmd("$qmiToolPath modem_info").submit { result ->
            val out = result.out.joinToString("\n")
            val err = result.err.joinToString("\n")
            if (result.isSuccess) {
                binding.tvLogs.text = "📊 DEVICE CAPABILITIES:\n$out"
            } else {
                binding.tvLogs.text = "❌ Diagnostic failed\nOUT: $out\nERR: $err"
            }
        }
    }

    private fun fetchMbnList() {
        if (qmiToolPath.isEmpty()) return
        binding.tvLogs.text = "📜 Scanning MBN Profiles..."
        Shell.cmd("$qmiToolPath list_mbns").submit { result ->
            val out = result.out.joinToString("\n")
            val err = result.err.joinToString("\n")
            if (result.isSuccess) {
                binding.tvLogs.text = "📁 STORED MBNs:\n$out"
            } else {
                binding.tvLogs.text = "❌ MBN Scan failed\nOUT: $out\nERR: $err"
            }
        }
    }

    /** Lock to NR bands (with optional LTE bands) using the general nr_lock command.
     *  Supports NR-CA: "n28+n78", "n28,n78", "n41+n78", etc. */
    private fun lockNrBands(nrSpec: String, lteMask: String?, label: String) {
        if (qmiToolPath.isEmpty()) {
            binding.tvLogs.text = "Error: Engine not found"
            return
        }
        binding.tvLogs.text = "Locking to $label (5G NR)..."
        val cmd = if (lteMask != null) {
            "$qmiToolPath nr_lock $nrSpec $lteMask"
        } else {
            "$qmiToolPath nr_lock $nrSpec"
        }
        Shell.cmd(cmd).submit { result ->
            val out = result.out.joinToString("\n")
            val err = result.err.joinToString("\n")
            if (result.isSuccess && out.contains("\"OK\"")) {
                binding.tvLogs.text = "✅ Locked to $label"
                binding.tvLockState.text = "🔒 LOCKED: $label"
                binding.tvLockState.setTextColor(0xFF06B6D4.toInt()) // cyan for 5G
            } else {
                binding.tvLogs.text = "❌ Lock failed\nOUT: $out\nERR: $err"
            }
            // Refresh signal info after lock
            handler.postDelayed({ updateSignalInfo() }, 1500)
        }
    }

    private fun startMonitoring() {
        handler.post(object : Runnable {
            override fun run() {
                updateSignalInfo()
                updateLockState()
                handler.postDelayed(this, 3000)
            }
        })
    }

    /** Query get_pref to show current locked band state */
    private fun updateLockState() {
        if (qmiToolPath.isEmpty()) return
        Shell.cmd("$qmiToolPath get_pref").submit { result ->
            if (result.isSuccess) {
                try {
                    val output = result.out.joinToString("")
                    // Find the JSON line with "preferences"
                    val jsonStr = output.lines().firstOrNull { it.trimStart().startsWith("{") } ?: output
                    val json = JSONObject(jsonStr)
                    val prefs = json.optJSONObject("preferences") ?: return@submit

                    val lteBandsHex = prefs.optString("lte_bands", "")
                    val nrBandList = prefs.optJSONArray("nr_band_list")
                    val modeName = prefs.optString("mode_name", "Unknown")
                    val modeHex = prefs.optString("mode", "0x0")

                    if (lteBandsHex.isNotEmpty()) {
                        val lteMask = java.lang.Long.decode(lteBandsHex).toULong()
                        val is5GEnabled = modeHex.contains("50") || modeHex.contains("5c") || modeHex.contains("40")

                        // Decode LTE bands
                        val lteBands = mutableListOf<String>()
                        val bandMap = mapOf(
                            1 to "B1", 2 to "B2", 3 to "B3", 4 to "B4", 5 to "B5",
                            7 to "B7", 8 to "B8", 12 to "B12", 13 to "B13", 14 to "B14",
                            17 to "B17", 18 to "B18", 19 to "B19", 20 to "B20",
                            25 to "B25", 26 to "B26", 28 to "B28", 29 to "B29",
                            30 to "B30", 32 to "B32", 34 to "B34", 38 to "B38",
                            39 to "B39", 40 to "B40", 41 to "B41", 42 to "B42",
                            43 to "B43", 46 to "B46", 48 to "B48", 66 to "B66"
                        )
                        for (b in 1..64) {
                            if (lteMask and (1UL shl (b - 1)) != 0UL) {
                                lteBands.add(bandMap[b] ?: "B$b")
                            }
                        }

                        // Decode NR bands
                        val nrBands = mutableListOf<String>()
                        if (is5GEnabled && nrBandList != null) {
                            if (nrBandList.length() > 10) {
                                nrBands.add("All NR")
                            } else {
                                for (i in 0 until nrBandList.length()) {
                                    nrBands.add(nrBandList.getString(i))
                                }
                            }
                        }

                        val isLteUnlocked = lteMask == ALL_BANDS_MASK || lteMask == 0xFFFFFFFFFFFFFFFFUL
                        val isNrUnlocked = nrBandList != null && nrBandList.length() > 10
                        
                        runOnUiThread {
                            if (isLteUnlocked && isNrUnlocked && modeName.contains("GSM")) {
                                binding.tvLockState.text = "🔓 UNLOCKED (All Bands)"
                                binding.tvLockState.setTextColor(0xFF10B981.toInt()) // emerald
                            } else {
                                val allBands = mutableListOf<String>()
                                if (!isLteUnlocked) allBands.addAll(lteBands)
                                if (is5GEnabled) allBands.addAll(nrBands)
                                
                                val label = if (allBands.isEmpty()) "NONE" else allBands.joinToString(" + ")
                                binding.tvLockState.text = "🔒 $modeName: $label"
                                
                                val color = if (is5GEnabled && !isNrUnlocked) 0xFF06B6D4.toInt() else 0xFFFBBF24.toInt()
                                binding.tvLockState.setTextColor(color)
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.e("QMI", "get_pref parse error", e)
                }
            }
        }
    }

    private fun updateSignalInfo() {
        if (qmiToolPath.isEmpty()) return

        // 1. Get QMI Data (cell_info outputs multiple JSON lines)
        Shell.cmd("$qmiToolPath cell_info").submit { result ->
            if (result.isSuccess) {
                try {
                    val lines = result.out
                    // First JSON line should be the cells data
                    for (line in lines) {
                        val trimmed = line.trim()
                        if (!trimmed.startsWith("{")) continue
                        try {
                            val json = JSONObject(trimmed)
                            // Parse cells array
                            val cells = json.optJSONArray("cells")
                            if (cells != null) {
                                val neighborInfo = StringBuilder()
                                for (i in 0 until cells.length()) {
                                    val cell = cells.getJSONObject(i)
                                    when (cell.getString("type")) {
                                        "serving" -> {
                                            runOnUiThread {
                                                binding.tvPci.text = cell.optString("pci", "--")
                                                binding.tvEarfcn.text = cell.optString("earfcn", "--")
                                            }
                                        }
                                        "neighbor" -> {
                                            val pci = cell.optInt("pci")
                                            val earfcn = cell.optInt("earfcn")
                                            val rsrp = cell.optInt("rsrp") / 10
                                            // Map EARFCN to band name
                                            val bandLabel = earfcnToBand(earfcn)
                                            neighborInfo.append("PCI: $pci | $bandLabel ($earfcn) | $rsrp dBm\n")
                                        }
                                        "nr_serving" -> {
                                            val pci = cell.optInt("pci")
                                            val rsrp = cell.optInt("rsrp")
                                            neighborInfo.append("📶 NR PCI: $pci | RSRP: $rsrp\n")
                                        }
                                    }
                                }
                                val finalNeighbor = neighborInfo.toString().trim()
                                runOnUiThread {
                                    binding.tvNeighbors.text = if (finalNeighbor.isEmpty()) "No extra towers detected" else finalNeighbor
                                }
                            }
                        } catch (_: Exception) { /* skip non-cell JSON lines */ }
                    }
                } catch (e: Exception) {
                    Log.e("QMI", "Parse error", e)
                }
            } else {
                Log.w("QMI", "QMI execution failed: ${result.err}")
            }
        }

        // 2. Get Dumpsys Data for signal strength and band
        Shell.cmd("dumpsys telephony.registry").submit { result ->
            if (result.isSuccess) {
                val output = result.out.joinToString("\n")
                parseDumpsys(output)
            }
        }
    }

    private fun earfcnToBand(earfcn: Int): String {
        return when {
            earfcn in 0..599 -> "B1"
            earfcn in 1200..1949 -> "B3"
            earfcn in 2750..3449 -> "B7"
            earfcn in 3450..3799 -> "B8"
            earfcn in 9210..9659 -> "B28"
            earfcn in 38650..39649 -> "B40"
            earfcn in 2400..2649 -> "B5"
            earfcn in 6150..6449 -> "B20"
            // NR-ARFCN ranges for 5G NR bands
            earfcn in 620000..653333 -> "n78"
            earfcn in 386000..398000 -> "n41"
            earfcn in 285400..286400 -> "n5"
            else -> "E$earfcn"
        }
    }

    private fun parseDumpsys(output: String) {
        val rsrpRegex = Regex("rsrp=(-?\\d+)")
        val bandRegex = Regex("mBands=\\[(\\d+)")
        val caRegex = Regex("isEndcAvailable = true|mIsUsingCarrierAggregation=true")

        val rsrpMatch = rsrpRegex.find(output)
        if (rsrpMatch != null) {
            val rsrpVal = rsrpMatch.groupValues[1]
            if (rsrpVal != "2147483647") {
                runOnUiThread { binding.tvRsrp.text = "$rsrpVal dBm" }
            }
        }

        val bandMatch = bandRegex.find(output)
        if (bandMatch != null) {
            runOnUiThread { binding.tvBand.text = "B${bandMatch.groupValues[1]}" }
        }

        runOnUiThread {
            if (caRegex.containsMatchIn(output)) {
                binding.tvCaBadge.visibility = View.VISIBLE
            } else {
                binding.tvCaBadge.visibility = View.GONE
            }
        }
    }
}
