using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

/// Build:
///   & "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /out:UsbIpAutoService.exe /r:System.ServiceProcess.dll UsbIpAutoService.cs
///
/// Install (run as admin):
///   sc create UsbIpAutoService binPath= "C:\path\to\UsbIpAutoService.exe" start= auto DisplayName= "USB/IP Auto Attach"
///   sc start UsbIpAutoService
///
/// Uninstall:
///   sc stop UsbIpAutoService
///   sc delete UsbIpAutoService
///
/// Logs: C:\ProgramData\UsbIpAutoService\service.log

public class UsbIpAutoService : ServiceBase
{
    private const string USBIP_EXE      = @"C:\Program Files\USBip\usbip.exe";
    private const string RPI_HOST       = "10.2.100.101";
    private const int    POLL_MS        = 10000;
    private const int    UDP_NOTIFY_PORT = 3241;
    private const string LOG_FILE       = @"C:\ProgramData\UsbIpAutoService\service.log";

    private Thread        pollingThread;
    private Thread        udpThread;
    private volatile bool running;
    private UdpClient     udpClient;

    public UsbIpAutoService()
    {
        ServiceName = "UsbIpAutoService";
        CanStop     = true;
        CanShutdown = true;
    }

    protected override void OnStart(string[] args) { Start(args); }
    protected override void OnStop()               { Stop(); }
    protected override void OnShutdown()           { Stop(); }

    public void Start(string[] args)
    {
        Log("Service starting");
        running = true;

        // Initial attach of all currently available devices
        try { AttachAll(); }
        catch (Exception ex) { Log("Initial attach error: " + ex.Message); }

        // UDP listener thread — reacts immediately when RPi broadcasts a new device
        udpThread = new Thread(UdpListenLoop);
        udpThread.IsBackground = true;
        udpThread.Name = "UsbIpUdpListener";
        udpThread.Start();

        // Polling thread — fallback in case a UDP packet is lost
        pollingThread = new Thread(PollLoop);
        pollingThread.IsBackground = true;
        pollingThread.Name = "UsbIpPoller";
        pollingThread.Start();
    }

    public new void Stop()
    {
        Log("Stopping - detaching all devices");
        running = false;
        try { if (udpClient != null) udpClient.Close(); } catch { }
        DetachAll();
    }

    // -------------------------------------------------------------------------
    // UDP listener: RPi broadcasts "ATTACH <busid>" when a device is bound
    // -------------------------------------------------------------------------

    private void UdpListenLoop()
    {
        try
        {
            udpClient = new UdpClient();
            udpClient.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udpClient.Client.Bind(new IPEndPoint(IPAddress.Any, UDP_NOTIFY_PORT));
            udpClient.EnableBroadcast = true;

            Log("UDP listener started on port " + UDP_NOTIFY_PORT);

            while (running)
            {
                IPEndPoint remote = new IPEndPoint(IPAddress.Any, 0);
                byte[] data;
                try { data = udpClient.Receive(ref remote); }
                catch { break; }

                // Only accept notifications from the configured RPi
                if (remote.Address.ToString() != RPI_HOST)
                    continue;

                string msg = Encoding.UTF8.GetString(data).Trim();
                Log("UDP received: " + msg);

                if (msg.StartsWith("ATTACH "))
                {
                    string busId = msg.Substring(7).Trim();
                    if (Regex.IsMatch(busId, @"^\d+-[\d.]+$"))
                    {
                        string output;
                        int exitCode = Run("attach", "-r " + RPI_HOST + " -b " + busId, out output);
                        if (exitCode == 0)
                            Log("Attached " + busId + " (via UDP notify)");
                        else if (!output.Contains("already"))
                            Log("attach " + busId + " failed: " + output.Trim());
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Log("UDP listener error: " + ex.Message);
        }
    }

    // -------------------------------------------------------------------------
    // Polling fallback — catches missed UDP packets
    // -------------------------------------------------------------------------

    private void PollLoop()
    {
        while (running)
        {
            Thread.Sleep(POLL_MS);
            try { AttachAll(); }
            catch (Exception ex) { Log("Poll error: " + ex.Message); }
        }
    }

    // -------------------------------------------------------------------------

    private void AttachAll()
    {
        List<string> busIds = ListRemoteDevices();
        foreach (string busId in busIds)
        {
            string output;
            int exitCode = Run("attach", "-r " + RPI_HOST + " -b " + busId, out output);
            if (exitCode == 0)
                Log("Attached " + busId);
            else if (!output.Contains("already"))
                Log("attach " + busId + " failed (exit " + exitCode + "): " + output.Trim());
        }
    }

    private void DetachAll()
    {
        string output;
        Run("detach", "--all", out output);
        Log("detach --all: " + output.Trim());
    }

    private List<string> ListRemoteDevices()
    {
        string output;
        Run("list", "-r " + RPI_HOST, out output);
        List<string> busIds = new List<string>();
        foreach (Match m in Regex.Matches(output, @"^\s+(\d+-[\d.]+)\s*:", RegexOptions.Multiline))
            busIds.Add(m.Groups[1].Value.Trim());
        return busIds;
    }

    private int Run(string cmd, string args, out string output)
    {
        ProcessStartInfo psi = new ProcessStartInfo(USBIP_EXE, cmd + " " + args);
        psi.UseShellExecute        = false;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError  = true;
        psi.CreateNoWindow         = true;

        using (Process proc = Process.Start(psi))
        {
            output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            return proc.ExitCode;
        }
    }

    // -------------------------------------------------------------------------

    private static readonly object logLock = new object();
    private static bool consoleMode = false;

    private void Log(string message)
    {
        string line = "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "] " + message;
        if (consoleMode)
            Console.WriteLine(line);
        lock (logLock)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LOG_FILE));
                File.AppendAllText(LOG_FILE, line + Environment.NewLine);
            }
            catch { }
        }
    }

    static void Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--console")
        {
            consoleMode = true;
            Console.WriteLine("Running in console mode. Press Ctrl+C to stop.");
            var svc = new UsbIpAutoService();
            svc.Start(args);
            Console.CancelKeyPress += (s, e) =>
            {
                e.Cancel = true;
                svc.Stop();
                Environment.Exit(0);
            };
            Thread.Sleep(Timeout.Infinite);
        }
        else
        {
            ServiceBase.Run(new UsbIpAutoService());
        }
    }
}
