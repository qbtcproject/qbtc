using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

// A bounded loopback-only adapter for this explicit-peer transport trial.
// Onion names are sent to Tor as SOCKS5 domain names, never resolved locally.
public sealed class QbtcOnionBridge : IDisposable {
    private readonly TcpListener listener;
    private readonly string onion;
    private Func<string> selector;
    private Action<string> failed;
    private readonly int proxyPort;
    private readonly SemaphoreSlim slots = new SemaphoreSlim(8, 8);
    private volatile bool stopped;
    private readonly object connectionGate=new object();
    private readonly HashSet<TcpClient> active=new HashSet<TcpClient>();
    public volatile string LastError = "";
    public static bool ValidOnion(string name) {
        return name != null && Regex.IsMatch(name, "\\A[a-z2-7]{56}\\.onion\\z");
    }
    private static byte[] ReadExact(Stream stream, int count) {
        byte[] bytes = new byte[count];
        int offset = 0;
        while (offset < count) {
            int read = stream.Read(bytes, offset, count - offset);
            if (read == 0) throw new IOException("Truncated SOCKS5 reply");
            offset += read;
        }
        return bytes;
    }
    public static TcpClient ConnectOnion(string name, int port) { return ConnectOnion(name, port, 19021, 60000); }
    public static TcpClient ConnectOnion(string name, int port, int destinationPort, int waitMs) {
        if (destinationPort != 19021 && destinationPort != 19023) throw new ArgumentException("QBTC service port required");
        if (!ValidOnion(name)) throw new ArgumentException("A Tor v3 .onion address is required");
        if (port < 1024 || port > 65535) throw new ArgumentException("Invalid local Tor SOCKS port");
        TcpClient client = new TcpClient();
        try {
            IAsyncResult pending = client.BeginConnect(IPAddress.Loopback, port, null, null);
            try {
                if (!pending.AsyncWaitHandle.WaitOne(5000)) throw new IOException("Local Tor SOCKS connection timed out");
                client.EndConnect(pending);
            } finally { pending.AsyncWaitHandle.Close(); }
            client.ReceiveTimeout = 5000; client.SendTimeout = 5000;
            NetworkStream stream = client.GetStream();
            stream.Write(new byte[] {5, 1, 0}, 0, 3);
            byte[] method = ReadExact(stream, 2);
            if (method[0] != 5 || method[1] != 0) throw new IOException("Tor SOCKS5 authentication method rejected");
            byte[] host = Encoding.ASCII.GetBytes(name);
            byte[] request = new byte[7 + host.Length];
            request[0] = 5; request[1] = 1; request[2] = 0; request[3] = 3;
            request[4] = (byte)host.Length; Buffer.BlockCopy(host, 0, request, 5, host.Length);
            request[5 + host.Length] = (byte)(destinationPort >> 8);
            request[6 + host.Length] = (byte)(destinationPort & 255);
            stream.Write(request, 0, request.Length);
            client.ReceiveTimeout = Math.Max(1000, Math.Min(waitMs, 60000));
            byte[] reply = ReadExact(stream, 4);
            if (reply[0] != 5 || reply[1] != 0 || reply[2] != 0) throw new IOException("Tor could not connect to the QBTC onion service (SOCKS5 reply " + reply[1] + ")");
            int length;
            if (reply[3] == 1) length = 4;
            else if (reply[3] == 4) length = 16;
            else if (reply[3] == 3) {
                length = ReadExact(stream, 1)[0];
                if (length == 0) throw new IOException("Invalid SOCKS5 domain reply");
            } else throw new IOException("Invalid SOCKS5 address type");
            ReadExact(stream, length + 2);
            return client;
        } catch { client.Close(); throw; }
    }
    public QbtcOnionBridge(int localPort, int torSocksPort, string peerOnion) {
        if (!ValidOnion(peerOnion)) throw new ArgumentException("A Tor v3 .onion address is required");
        if (localPort < 1024 || localPort > 65535 || localPort == 19021 || localPort == 19121 || localPort == torSocksPort) throw new ArgumentException("Invalid bridge listener port");
        onion = peerOnion; proxyPort = torSocksPort;
        listener = new TcpListener(IPAddress.Loopback, localPort);
        listener.ExclusiveAddressUse = true;
        listener.Start(8);
        Thread thread = new Thread(Accept); thread.IsBackground = true; thread.Start();
    }
    public QbtcOnionBridge(int localPort, int torSocksPort, Func<string> select, Action<string> onFailure) : this(localPort, torSocksPort, new string('a',56)+".onion") { selector=select; failed=onFailure; }
    private void Accept() {
        while (!stopped) {
            TcpClient incoming;
            try { incoming = listener.AcceptTcpClient(); } catch { break; }
            if (!slots.Wait(0)) { incoming.Close(); continue; }
            lock(connectionGate){if(stopped){incoming.Close();slots.Release();break;}active.Add(incoming);}
            ThreadPool.QueueUserWorkItem(delegate { Relay(incoming); });
        }
    }
    private void Relay(TcpClient incoming) {
        TcpClient outgoing = null;
        string target = onion;
        try {
            incoming.ReceiveTimeout = 30000; incoming.SendTimeout = 30000;
            target = selector == null ? onion : selector();
            if (!ValidOnion(target)) throw new IOException("No reachable QBTC peer yet");
            outgoing = ConnectOnion(target, proxyPort);
            lock(connectionGate){if(stopped){outgoing.Close();throw new IOException("Bridge stopped");}active.Add(outgoing);}
            TcpClient remote = outgoing;
            Task one = Task.Factory.StartNew(delegate { Pump(incoming.GetStream(), remote.GetStream()); });
            Task two = Task.Factory.StartNew(delegate { Pump(remote.GetStream(), incoming.GetStream()); });
            Task.WaitAny(one, two);
            incoming.Close(); remote.Close();
            try { Task.WaitAll(new Task[] {one, two}, 1000); } catch { }
        } catch (Exception error) { LastError = error.Message; if(failed != null && ValidOnion(target)) failed(target); }
        finally { incoming.Close(); if (outgoing != null) outgoing.Close(); lock(connectionGate){active.Remove(incoming);if(outgoing!=null)active.Remove(outgoing);} slots.Release(); }
    }
    private static void Pump(Stream source, Stream destination) {
        byte[] buffer = new byte[16384];
        try {
            int count;
            while ((count = source.Read(buffer, 0, buffer.Length)) > 0) destination.Write(buffer, 0, count);
        } catch (IOException) { } catch (ObjectDisposedException) { }
    }
    public void Dispose() { lock(connectionGate){stopped = true; listener.Stop();foreach(var c in active.ToArray())c.Close();} }
}
