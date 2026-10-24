using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Net;
using System.Net.Sockets;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Threading;

// Bounded onion address exchange, separate from the unchanged native consensus wire.
// An advertised address is only a candidate. Share it after an outbound native-mainnet status check.
public sealed class QbtcOnionNetwork : IDisposable {
    public const string Genesis = "000006d71412362513fb97c48c0a1371b0edd3e7e6a29e91a8318f889181e5dd1255621ebcc717083a1a135c57c12b6c3885e2e0dbfac410d43089fbb49ed73c";
    private const string Magic = "QBTC-ONION-1 mainnet " + Genesis;
    private sealed class Entry {
        public string Name; public bool Seed; public int Failures; public bool Incoming;
        public string Source; public DateTime FirstGood = DateTime.MinValue;
        public DateTime Good = DateTime.MinValue, Due = DateTime.MinValue;
    }
    private readonly object gate = new object();
    private readonly Dictionary<string,Entry> book = new Dictionary<string,Entry>(StringComparer.Ordinal);
    private readonly HashSet<TcpClient> clients = new HashSet<TcpClient>();
    private readonly Dictionary<string,IntroductionBudget> learned = new Dictionary<string,IntroductionBudget>();
    private sealed class IntroductionBudget { public int Count; public DateTime Until; }
    private readonly byte[] selectionSalt = new byte[32];
    private readonly string[] preferred = new string[4];
    private DateTime incomingReset = DateTime.MinValue; private int incomingBudget;
    private readonly string own, path; private readonly int socks;
    private readonly TcpListener listener; private readonly ManualResetEvent quit = new ManualResetEvent(false);
    private readonly SemaphoreSlim incomingSlots = new SemaphoreSlim(8,8);
    private readonly List<QbtcOnionBridge> bridges = new List<QbtcOnionBridge>();
    private bool stopped; private int rotation;
    public volatile string LastError = "";
    public QbtcOnionNetwork(string ownName, string store, int socksPort, string[] initial, string manual) {
        if(!QbtcOnionBridge.ValidOnion(ownName)) throw new ArgumentException("Invalid local onion address");
        using(var rng=RandomNumberGenerator.Create())rng.GetBytes(selectionSalt);
        own=ownName; path=store; socks=socksPort;
        Load();
        foreach(string name in (initial ?? new string[0]).Take(8)) Add(name,true,null);
        if(!String.IsNullOrEmpty(manual) && manual!=own) Add(manual,true,null);
        listener = new TcpListener(IPAddress.Loopback,19023);
        try {
            listener.ExclusiveAddressUse = true;
            listener.Start(8);
            for(int lane=0;lane<4;lane++){int selectedLane=lane;bridges.Add(new QbtcOnionBridge(19022+lane*2,socks,delegate{return SelectLane(selectedLane);},Failed));}
            Thread accept = new Thread(Accept); accept.IsBackground=true;accept.Start();
            for(int i=0;i<2;i++){Thread worker = new Thread(Work);worker.IsBackground=true;worker.Start();}
        } catch { listener.Stop();foreach(var b in bridges)b.Dispose(); throw; }
    }
    public int KnownCount { get { lock(gate) return book.Count; } }
    public int VerifiedCount { get { lock(gate) return book.Values.Count(e=>Fresh(e)); } }
    private bool Fresh(Entry e) { return e.Good > DateTime.UtcNow.AddMinutes(-5) && e.Failures==0; }
    public string[] SelectedPeers { get {lock(gate){for(int i=0;i<4;i++)SelectLocked(i);return preferred.Where(n=>n!=null).ToArray();}} }
    public string CurrentPeer { get { lock(gate) return SelectLocked(); } }
    private string Rank(string name) {
        using(var h=SHA256.Create())return BitConverter.ToString(h.ComputeHash(selectionSalt.Concat(Encoding.ASCII.GetBytes(name)).ToArray()));
    }
    private string SelectLocked(int lane=0) {
        Entry existing;
        if(preferred[lane]!=null && book.TryGetValue(preferred[lane],out existing) && Fresh(existing))return preferred[lane];
        preferred[lane]=null;
        var used=new HashSet<string>(preferred.Where(n=>n!=null),StringComparer.Ordinal);
        // Stable lanes use distinct services; random per-process rank avoids global-clock herding.
        // Long-lived contacts win replacement slots. Reachability is NOT proof of honest history.
        var next=book.Values.Where(e=>Fresh(e) && !used.Contains(e.Name))
            .OrderByDescending(e=>e.Seed).ThenByDescending(e=>(int)(DateTime.UtcNow-e.FirstGood).TotalDays).ThenBy(e=>Rank(e.Name),StringComparer.Ordinal).FirstOrDefault();
        if(next==null)return "";
        preferred[lane]=next.Name;return next.Name;
    }
    private string SelectLane(int lane) { lock(gate) return SelectLocked(lane); }
    private bool Add(string name,bool seed,string source) {
        if(!QbtcOnionBridge.ValidOnion(name) || name==own)return false;
        lock(gate) {
            if(stopped)return false;
            Entry existing;
            if(book.TryGetValue(name,out existing)){if(seed)existing.Seed=true;return false;}
            if(source=="incoming") {
                var pending=book.Values.Where(e=>e.Incoming && !e.Seed && !Fresh(e)).ToArray();
                if(pending.Length>=8){var old=pending.Where(e=>e.Failures>0).OrderByDescending(e=>e.Failures).FirstOrDefault();if(old==null)return false;book.Remove(old.Name);}
            } else if(source!=null) {
                var now=DateTime.UtcNow;
                foreach(var expired in learned.Where(k=>k.Value.Until<=now).Select(k=>k.Key).ToArray())learned.Remove(expired);
                IntroductionBudget quota;
                if(learned.TryGetValue(source,out quota) && quota.Count>=4)return false;
                if(!learned.ContainsKey(source) && learned.Count>=64)return false;
                if(book.Values.Count(e=>e.Source==source)>=4)return false;
            }
            if(book.Count>=64) {
                var victim=book.Values.Where(e=>!e.Seed && !Fresh(e)).OrderByDescending(e=>e.Failures).FirstOrDefault();
                if(victim==null)return false;
                book.Remove(victim.Name);
            }
            book.Add(name,new Entry{Name=name,Seed=seed,Incoming=source=="incoming",Source=source});
            if(source!=null && source!="incoming"){
                IntroductionBudget quota;if(!learned.TryGetValue(source,out quota)){quota=new IntroductionBudget{Until=DateTime.UtcNow.AddHours(1)};learned[source]=quota;}quota.Count++;
            }
            return true;
        }
    }
    private bool Track(TcpClient c) { lock(gate) {if(stopped){c.Close();return false;} clients.Add(c);return true;} }
    private void Untrack(TcpClient c) { lock(gate) clients.Remove(c);c.Close(); }
    private static string ReadLine(Stream s, DateTime until) {
        var b=new List<byte>();
        while(b.Count<=190) {
            int remaining=(int)(until-DateTime.UtcNow).TotalMilliseconds;
            if(remaining<=0)throw new IOException("Onion exchange deadline");
            s.ReadTimeout=Math.Min(remaining,5000);
            int x=s.ReadByte();if(x<0)throw new IOException("Truncated onion exchange");
            if(x==10)return Encoding.ASCII.GetString(b.ToArray());
            if(x<32 || x>126)throw new IOException("Invalid onion exchange character");
            b.Add((byte)x);
        }
        throw new IOException("Onion exchange line limit");
    }
    private static string[] ReadMessage(Stream s, DateTime until) {
        if(ReadLine(s,until)!=Magic)throw new IOException("Wrong onion network or genesis");
        string service=ReadLine(s,until);
        if(!QbtcOnionBridge.ValidOnion(service))throw new IOException("Invalid advertised onion");
        int n; string count=ReadLine(s,until);
        if(!Int32.TryParse(count,out n)||n<0||n>8||count!=n.ToString())throw new IOException("Onion list count limit");
        var names=new List<string>{service};
        for(int i=0;i<n;i++) {
            string name=ReadLine(s,until);
            if(!QbtcOnionBridge.ValidOnion(name) || names.Contains(name))throw new IOException("Invalid or duplicate onion candidate");
            names.Add(name);
        }
        return names.ToArray();
    }
    private void WriteMessage(Stream s, bool share) {
        string[] names;
        lock(gate) {
            var good=book.Values.Where(e=>Fresh(e)).Select(e=>e.Name).OrderBy(n=>n,StringComparer.Ordinal).ToArray();
            names=share ? good.Skip(good.Length==0?0:rotation++%good.Length).Concat(good).Distinct().Take(8).ToArray() : new string[0];
        }
        byte[] b=Encoding.ASCII.GetBytes(Magic+"\n"+own+"\n"+names.Length+"\n"+String.Join("\n",names)+(names.Length>0?"\n":""));
        s.WriteTimeout=5000;s.Write(b,0,b.Length);
    }
    private void Accept() {
        while(!quit.WaitOne(0)) {
            TcpClient c;try{c=listener.AcceptTcpClient();}catch{break;}
            bool admitted;
            lock(gate){var now=DateTime.UtcNow;if(now>=incomingReset){incomingReset=now.AddSeconds(1);incomingBudget=8;}admitted=incomingBudget>0;if(admitted)incomingBudget--;}
            if(!admitted || !incomingSlots.Wait(0)){c.Close();continue;}
            if(!Track(c)){incomingSlots.Release();break;}
            ThreadPool.QueueUserWorkItem(delegate {
                try {
                    var s=c.GetStream();var names=ReadMessage(s,DateTime.UtcNow.AddSeconds(3));
                    // Unauthenticated callers cannot inject a list. Their own service is only a candidate.
                    if(names.Length!=1)throw new IOException("Incoming discovery accepts only self announcement");
                    Add(names[0],false,"incoming");WriteMessage(s,true);
                } catch(Exception e){LastError=e.Message;}
                finally{Untrack(c);incomingSlots.Release();}
            });
        }
    }
    private static byte[] Exact(Stream s,int n) {
        byte[] b=new byte[n];int at=0;
        DateTime end=DateTime.UtcNow.AddSeconds(8);
        while(at<n){int remain=(int)(end-DateTime.UtcNow).TotalMilliseconds;if(remain<=0)throw new IOException("Status deadline");s.ReadTimeout=Math.Min(remain,5000);int got=s.Read(b,at,n-at);if(got==0)throw new IOException("Truncated native status");at+=got;}return b;
    }
    private void Probe(string name) {
        var c=QbtcOnionBridge.ConnectOnion(name,socks,19021,30000);
        if(!Track(c))throw new IOException("Network stopped");
        try {
            var s=c.GetStream();byte[] request=new byte[]{0x32,0x4d,0x42,0x51,1,0,0,0,(byte)'S',0,0,0,0};
            s.WriteTimeout=5000;s.Write(request,0,request.Length);
            byte[] h=Exact(s,13);
            for(int i=0;i<9;i++)if(h[i]!=request[i])throw new IOException("Wrong native network, wire or response");
            uint n=(uint)(h[9]|(h[10]<<8)|(h[11]<<16)|(h[12]<<24));if(n==0||n>2048)throw new IOException("Native status length limit");
            string text=new UTF8Encoding(false,true).GetString(Exact(s,(int)n));
            if(!text.StartsWith("network=mainnet height=",StringComparison.Ordinal))throw new IOException("Native mainnet status required");
        } finally {Untrack(c);}
    }
    private void Exchange(string name) {
        var c=QbtcOnionBridge.ConnectOnion(name,socks,19023,30000);
        if(!Track(c))throw new IOException("Network stopped");
        try {
            var s=c.GetStream();WriteMessage(s,false);var names=ReadMessage(s,DateTime.UtcNow.AddSeconds(8));
            if(names[0]!=name)throw new IOException("Onion service identity mismatch");
            // Only after full bounded message validation add up to four candidates from this source.
            foreach(string candidate in names.Skip(1))Add(candidate,false,name);
        } finally {Untrack(c);}
    }
    private void Failed(string name) {
        lock(gate) {
            Entry e;if(!book.TryGetValue(name,out e))return;
            e.Failures=Math.Min(e.Failures+1,8);e.Good=DateTime.MinValue;
            e.Due=DateTime.UtcNow.AddSeconds(Math.Min(300,2*(1<<e.Failures)));
        }
    }
    private void Work() {
        while(!quit.WaitOne(250)) {
            Entry next;
            lock(gate){next=book.Values.Where(e=>e.Due<=DateTime.UtcNow).OrderByDescending(e=>e.Good!=DateTime.MinValue).ThenBy(e=>e.Due).ThenByDescending(e=>e.Seed).FirstOrDefault();if(next!=null)next.Due=DateTime.UtcNow.AddSeconds(90);}
            if(next==null)continue;
            try {
                Probe(next.Name);
                lock(gate){if(next.FirstGood==DateTime.MinValue)next.FirstGood=DateTime.UtcNow;next.Good=DateTime.UtcNow;next.Failures=0;next.Due=DateTime.UtcNow.AddSeconds(45);}
                try{Exchange(next.Name);}catch(Exception e){LastError="Peer reachable; address exchange unavailable: "+e.Message;}
            }catch(Exception e){LastError=e.Message;Failed(next.Name);continue;}
            try{Save();}catch(Exception e){LastError="Peer reachable; local address store could not be saved: "+e.Message;}
        }
    }
    private static string Digest(string text) {using(var hash=SHA256.Create())return BitConverter.ToString(hash.ComputeHash(Encoding.ASCII.GetBytes(text))).Replace("-","").ToLowerInvariant();}
    private void Load() {
        if(!File.Exists(path))return;
        try {
            if(new FileInfo(path).Length>16384)throw new IOException("Onion store size limit");
            var lines=File.ReadAllLines(path);if(lines.Length<2||lines.Length>66||(lines[0]!="QBTC-ONION-STORE-1 "+Genesis && lines[0]!="QBTC-ONION-STORE-2 "+Genesis))throw new IOException("Onion store format");
            string body=String.Join("\n",lines.Skip(2))+"\n";
            if(lines[1]!=Digest(body))throw new IOException("Onion store checksum");
            bool v2=lines[0]=="QBTC-ONION-STORE-2 "+Genesis;
            var records=lines.Skip(2).Select(line=>line.Split('\t')).ToArray();
            if(records.Any(parts=>parts.Length!=(v2?2:1) || !QbtcOnionBridge.ValidOnion(parts[0]) || (v2 && parts[1]!="-" && parts[1]!="incoming" && !QbtcOnionBridge.ValidOnion(parts[1]))) || records.Select(parts=>parts[0]).Distinct().Count()!=records.Length)throw new IOException("Invalid stored onion list");
            foreach(var parts in records)Add(parts[0],false,v2 && parts[1]!="-"?parts[1]:null);

        }catch(Exception e){LastError="Ignoring onion address store: "+e.Message;}
    }
    private void Save() {
        lock(gate) {
            if(stopped)return;
            // Persist successful contacts first for restart independence; revalidate on each run.
            var names=book.Values.OrderByDescending(e=>Fresh(e)).ThenBy(e=>e.Failures).Select(e=>e.Name+"\t"+(e.Source??"-")).ToArray();
            string body=String.Join("\n",names)+"\n";
            string text="QBTC-ONION-STORE-2 "+Genesis+"\n"+Digest(body)+"\n"+body;
            string temp=path+".tmp";File.WriteAllText(temp,text,Encoding.ASCII);
            if(File.Exists(path))File.Replace(temp,path,path+".bak");else File.Move(temp,path);
        }
    }
    public void Dispose() {
        lock(gate){if(stopped)return;stopped=true;quit.Set();listener.Stop();foreach(var c in clients.ToArray())c.Close();}
        foreach(var b in bridges)b.Dispose();
    }
}
