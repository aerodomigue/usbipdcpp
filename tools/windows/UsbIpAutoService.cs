using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

/// Build:
///   & "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /out:UsbIpAutoService.exe /r:System.ServiceProcess.dll UsbIpAutoService.cs
///
/// Install (run as admin):
///   sc create UsbIpAutoService binPath= "C:\path\to\UsbIpAutoService.exe" start= auto DisplayName= "USB/IP Auto Attach"
///   sc triggerinfo UsbIpAutoService start/networkon    <- start only when network is up
///   sc start UsbIpAutoService
///
/// Uninstall:
///   sc stop UsbIpAutoService && sc delete UsbIpAutoService
///
/// Logs: C:\ProgramData\UsbIpAutoService\service.log

public class UsbIpAutoService : ServiceBase
{
    private const string USBIP_EXE          = @"C:\Program Files\USBip\usbip.exe";
    private const string RPI_HOST           = "10.2.100.101";
    private const int    RPI_PORT           = 3240;
    private const int    POLL_MS            = 10000;
    private const int    UDP_NOTIFY_PORT    = 3241;
    private const int    NET_PROBE_MS       = 5000;
    private const int    NET_PROBE_MAX      = 36;       // 36 x 5s = 3 min
    private const int    RUN_TIMEOUT_MS     = 8000;     // max time for each usbip.exe call
    private const int    SHUTDOWN_EXTRA_MS  = 15000;    // extra time to request from SCM
    private const string LOG_FILE           = @"C:\ProgramData\UsbIpAutoService\service.log";

    private Thread        pollingThread;
    private Thread        udpThread;
    private Thread        networkWaitThread;
    private volatile bool running;
    private UdpClient     udpClient;
    private int           shutdownOnce;   // Interlocked flag — ensures Shutdown() runs exactly once

    public UsbIpAutoService()
    {
        ServiceName         = "UsbIpAutoService";
        CanStop             = true;
        CanShutdown         = true;
        CanHandlePowerEvent = true;

        try { Directory.CreateDirectory(Path.GetDirectoryName(LOG_FILE)); }
        catch { /* log dir creation failure is non-fatal */ }
    }

    // -------------------------------------------------------------------------
    // Service lifecycle
    // -------------------------------------------------------------------------

    protected override void OnStart(string[] args)
    {
        Log("Service starting");
        running       = true;
        shutdownOnce  = 0;

        // Wire network address change event (fires on DHCP assign, IP change, wake-from-sleep)
        // Used as an additional trigger to re-attempt attach without waiting for the poll interval.
        NetworkChange.NetworkAddressChanged += OnNetworkAddressChanged;

        // Create UDP socket BEFORE starting the thread so Stop() can always close it
        try
        {
            udpClient = new UdpClient();
            udpClient.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udpClient.Client.Bind(new IPEndPoint(IPAddress.Any, UDP_NOTIFY_PORT));
            udpClient.EnableBroadcast = true;
        }
        catch (Exception ex)
        {
            Log("UDP bind failed on port " + UDP_NOTIFY_PORT + ": " + ex.Message + " — hotplug notifications disabled");
            if (udpClient != null) { try { udpClient.Dispose(); } catch { } udpClient = null; }
        }

        udpThread = new Thread(UdpListenLoop) { IsBackground = true, Name = "UsbIpUdpListener" };
        udpThread.Start();

        pollingThread = new Thread(PollLoop) { IsBackground = true, Name = "UsbIpPoller" };
        pollingThread.Start();

        // Initial attach: probe until RPi is reachable
        TriggerNetworkWait();
    }

    protected override void OnStop()     { Shutdown(); }
    protected override void OnShutdown() { Shutdown(); }

    // OnPowerEvent: called on sleep/wake. Return value is ignored by OS for non-query events.
    protected override bool OnPowerEvent(PowerBroadcastStatus powerStatus)
    {
        switch (powerStatus)
        {
            case PowerBroadcastStatus.Suspend:
                // Detach before suspend. The OS does NOT wait for this handler,
                // so use a short timeout — better a partial detach than a hang.
                Log("System going to sleep - detaching all devices");
                DetachAll(timeoutMs: 6000);
                break;

            case PowerBroadcastStatus.ResumeSuspend:
            case PowerBroadcastStatus.ResumeAutomatic:
                Log("System waking up - will attach when network is ready");
                TriggerNetworkWait();
                break;
        }
        return true;
    }

    public void Shutdown()
    {
        // Guard: run exactly once even if OnStop + OnShutdown both fire
        if (Interlocked.Exchange(ref shutdownOnce, 1) != 0) return;

        Log("Shutdown - detaching all devices");
        running = false;

        NetworkChange.NetworkAddressChanged -= OnNetworkAddressChanged;

        // Request extra time from SCM so usbip.exe can finish
        try { RequestAdditionalTime(SHUTDOWN_EXTRA_MS); } catch { }

        // Close UDP socket to unblock Receive()
        try { if (udpClient != null) { udpClient.Dispose(); udpClient = null; } } catch { }

        DetachAll(timeoutMs: 10000);
    }

    // -------------------------------------------------------------------------
    // Network change event — triggers a re-attach attempt when IP changes
    // -------------------------------------------------------------------------

    private void OnNetworkAddressChanged(object sender, EventArgs e)
    {
        if (!running) return;
        Log("Network address changed - checking RPi reachability");
        TriggerNetworkWait();
    }

    // -------------------------------------------------------------------------
    // Network wait: probe RPi until reachable, then attach. One thread at a time.
    // -------------------------------------------------------------------------

    private readonly object networkWaitLock = new object();

    private void TriggerNetworkWait()
    {
        lock (networkWaitLock)
        {
            // If a wait is already running let it finish — AttachAll is idempotent
            if (networkWaitThread != null && networkWaitThread.IsAlive) return;

            networkWaitThread = new Thread(WaitForNetworkThenAttach)
                { IsBackground = true, Name = "UsbIpNetworkWaiter" };
            networkWaitThread.Start();
        }
    }

    private void WaitForNetworkThenAttach()
    {
        for (int attempt = 0; attempt < NET_PROBE_MAX && running; attempt++)
        {
            try
            {
                using (TcpClient tcp = new TcpClient())
                {
                    IAsyncResult ar = tcp.BeginConnect(RPI_HOST, RPI_PORT, null, null);
                    if (ar.AsyncWaitHandle.WaitOne(NET_PROBE_MS) && tcp.Connected)
                    {
                        try { tcp.EndConnect(ar); } catch { }
                        Log("RPi reachable - attaching devices");
                        try { AttachAll(); }
                        catch (Exception ex) { Log("AttachAll error: " + ex.Message); }
                        return;
                    }
                }
            }
            catch { /* probe failed, keep trying */ }

            if (attempt == 0)
                Log("Waiting for RPi at " + RPI_HOST + ":" + RPI_PORT + "...");

            if (running) Thread.Sleep(NET_PROBE_MS);
        }

        if (running)
            Log("Could not reach RPi after " + NET_PROBE_MAX + " attempts");
    }

    // -------------------------------------------------------------------------
    // UDP listener: RPi broadcasts "ATTACH <busid>" on device hotplug
    // -------------------------------------------------------------------------

    private void UdpListenLoop()
    {
        if (udpClient == null) return;

        IPAddress rpiAddr;
        try { rpiAddr = IPAddress.Parse(RPI_HOST); }
        catch (Exception ex) { Log("Invalid RPI_HOST address: " + ex.Message); return; }

        Log("UDP listener ready on port " + UDP_NOTIFY_PORT);

        while (running)
        {
            IPEndPoint remote = new IPEndPoint(IPAddress.Any, 0);
            byte[] data;

            try { data = udpClient.Receive(ref remote); }
            catch (Exception ex)
            {
                if (!running) break;  // socket closed intentionally by Shutdown()
                Log("UDP receive error (will retry): " + ex.Message);
                Thread.Sleep(1000);
                continue;
            }

            if (!rpiAddr.Equals(remote.Address)) continue;

            string msg;
            try { msg = Encoding.UTF8.GetString(data).Trim(); }
            catch { continue; }

            Log("UDP received: " + msg);

            if (msg.StartsWith("ATTACH "))
            {
                string busId = msg.Substring(7).Trim();
                if (!Regex.IsMatch(busId, @"^\d+-[\d.]+$")) continue;

                try
                {
                    string output;
                    int exitCode = Run("attach", "-r " + RPI_HOST + " -b " + busId, out output);
                    if (exitCode == 0)
                        Log("Attached " + busId + " (UDP notify)");
                    else if (!output.Contains("already"))
                        Log("attach " + busId + " failed: " + output.Trim());
                }
                catch (Exception ex) { Log("attach " + busId + " exception: " + ex.Message); }
            }
        }

        Log("UDP listener stopped");
    }

    // -------------------------------------------------------------------------
    // Polling fallback
    // -------------------------------------------------------------------------

    private void PollLoop()
    {
        while (running)
        {
            Thread.Sleep(POLL_MS);  // sleep first: startup attach is handled by TriggerNetworkWait
            if (!running) break;
            try { AttachAll(); }
            catch (Exception ex) { Log("Poll error: " + ex.Message); }
        }
    }

    // -------------------------------------------------------------------------
    // USB/IP operations
    // -------------------------------------------------------------------------

    private void AttachAll()
    {
        List<string> busIds;
        try { busIds = ListRemoteDevices(); }
        catch (Exception ex) { Log("list failed: " + ex.Message); return; }

        foreach (string busId in busIds)
        {
            try
            {
                string output;
                int exitCode = Run("attach", "-r " + RPI_HOST + " -b " + busId, out output);
                if (exitCode == 0)
                    Log("Attached " + busId);
                else if (!output.Contains("already"))
                    Log("attach " + busId + " failed (exit " + exitCode + "): " + output.Trim());
            }
            catch (Exception ex) { Log("attach " + busId + " exception: " + ex.Message); }
        }
    }

    private void DetachAll(int timeoutMs = RUN_TIMEOUT_MS)
    {
        try
        {
            string output;
            int exitCode = Run("detach", "--all", out output, timeoutMs);
            Log("detach --all (exit " + exitCode + "): " + output.Trim());
        }
        catch (Exception ex) { Log("detach --all exception: " + ex.Message); }
    }

    private List<string> ListRemoteDevices()
    {
        string output;
        Run("list", "-r " + RPI_HOST, out output);
        var busIds = new List<string>();
        foreach (Match m in Regex.Matches(output, @"^\s+(\d+-[\d.]+)\s*:", RegexOptions.Multiline))
            busIds.Add(m.Groups[1].Value.Trim());
        return busIds;
    }

    // Spawn usbip.exe with concurrent stdout/stderr reads and a hard timeout
    private int Run(string cmd, string args, out string output, int timeoutMs = RUN_TIMEOUT_MS)
    {
        ProcessStartInfo psi = new ProcessStartInfo(USBIP_EXE, cmd + " " + args);
        psi.UseShellExecute        = false;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError  = true;
        psi.CreateNoWindow         = true;

        Process proc;
        try { proc = Process.Start(psi); }
        catch (Exception ex) { output = "Failed to start usbip.exe: " + ex.Message; return -1; }

        if (proc == null) { output = "Process.Start returned null"; return -1; }

        using (proc)
        {
            // Read stdout and stderr concurrently to avoid pipe buffer deadlock
            string stdout = null, stderr = null;
            Task t1 = Task.Run(() => { try { stdout = proc.StandardOutput.ReadToEnd(); } catch { } });
            Task t2 = Task.Run(() => { try { stderr = proc.StandardError.ReadToEnd(); } catch { } });

            bool exited = proc.WaitForExit(timeoutMs);
            if (!exited)
            {
                try { proc.Kill(); } catch { }
                Task.WaitAll(new[] { t1, t2 }, 2000);
                output = (stdout ?? "") + (stderr ?? "") + " [TIMEOUT after " + timeoutMs + "ms]";
                Log("usbip.exe " + cmd + " timed out");
                return -2;
            }

            Task.WaitAll(new[] { t1, t2 }, 2000);
            output = (stdout ?? "") + (stderr ?? "");
            return proc.ExitCode;
        }
    }

    // -------------------------------------------------------------------------
    // Logging — fallback to Windows Event Log if file write fails
    // -------------------------------------------------------------------------

    private static readonly object logLock    = new object();
    private static          bool   consoleMode = false;

    private void Log(string message)
    {
        string line = "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "] " + message;
        if (consoleMode) Console.WriteLine(line);
        lock (logLock)
        {
            try { File.AppendAllText(LOG_FILE, line + Environment.NewLine); }
            catch (Exception ex)
            {
                try
                {
                    if (!EventLog.SourceExists(ServiceName))
                        EventLog.CreateEventSource(ServiceName, "Application");
                    EventLog.WriteEntry(ServiceName,
                        "Log write failed (" + ex.Message + "): " + message,
                        EventLogEntryType.Warning);
                }
                catch { }
            }
        }
    }

    // -------------------------------------------------------------------------

    static void Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--console")
        {
            consoleMode = true;
            Console.WriteLine("Running in console mode. Press Ctrl+C to stop.");
            var svc = new UsbIpAutoService();
            svc.OnStart(args);
            Console.CancelKeyPress += (s, e) => { e.Cancel = true; svc.Shutdown(); Environment.Exit(0); };
            Thread.Sleep(Timeout.Infinite);
        }
        else
        {
            ServiceBase.Run(new UsbIpAutoService());
        }
    }
}
