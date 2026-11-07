// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Collections.Generic;
using Nikki.Utils;
namespace Undercoop.Binary {
internal sealed class LauncherSelectionException:FormatException {
    internal readonly string[] Launchers;
    internal LauncherSelectionException(IEnumerable<string> entries):base("Multiple compatible Binary installers were found. Choose the intended installation entry."){Launchers=entries.ToArray();}
}
internal sealed class InstallPlanner {
    internal readonly HashSet<string> Used=new HashSet<string>(StringComparer.Ordinal);
    internal readonly List<string[]> Copies=new List<string[]>();
    internal readonly List<string> Notes=new List<string>();
    internal readonly HashSet<string> Required=new HashSet<string>();
    internal string Launcher,Name="Binary mod";
    readonly string input;readonly Workspace workspace;readonly bool cook;readonly HashSet<string> visiting=new HashSet<string>();int commands,dataEdits;
    internal InstallPlanner(string root,Workspace w,bool build){input=root;workspace=w;cook=build;}
    string Source(string directory,string path){
        if(Path.IsPathRooted(path)||path.Contains(':'))throw new FormatException("Absolute installer source paths are not supported.");
        string full=Path.GetFullPath(Path.Combine(input,directory.Replace('/',Path.DirectorySeparatorChar),path.Replace('\\',Path.DirectorySeparatorChar)));
        if(!full.StartsWith(input+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new FormatException("Installer include escaped its archive.");
        string relative=Guard.PathName(full.Substring(input.Length+1));string actual=Guard.Within(input,relative);if(!File.Exists(actual))throw new FormatException("Installer resource is missing: "+relative);Used.Add(relative);return relative;
    }
    JsonDocument LauncherJson(string path){
        string text=Encoding.UTF8.GetString(Guard.Read(Guard.Within(input,path),262144)).TrimStart('\uFEFF');int newline=text.IndexOf('\n');
        if(newline<0||text.Substring(0,newline).Trim()!="[VERSN1]")throw new FormatException("Expected a bounded VERSN1 launcher.");
        return JsonDocument.Parse(text.Substring(newline+1).Replace("\\","\\\\"),new JsonDocumentOptions{AllowTrailingCommas=true,CommentHandling=JsonCommentHandling.Skip,MaxDepth=12});
    }
    bool UserLauncher(string path){
        using var json=LauncherJson(path);var root=json.RootElement;
        bool user=root.ValueKind==JsonValueKind.Object&&root.TryGetProperty("Usage",out var usage)&&usage.ValueKind==JsonValueKind.String&&usage.GetString()=="User"&&
            root.TryGetProperty("Game",out var game)&&game.ValueKind==JsonValueKind.String&&game.GetString()=="Underground2";
        if(user&&(!root.TryGetProperty("Endscript",out var script)||script.ValueKind!=JsonValueKind.String||string.IsNullOrWhiteSpace(script.GetString())))throw new FormatException("The Binary User-mode launcher has no valid Endscript path.");
        return user;
    }
    internal void Read(string launcher=null){
        string requested=launcher==null?null:Guard.PathName(launcher);
        var files=Directory.GetFiles(input,"*.end",SearchOption.AllDirectories);
        var launchers=files.Where(p=>new FileInfo(p).Length<=262144&&File.ReadLines(p).FirstOrDefault()?.Trim('\uFEFF',' ','\t')=="[VERSN1]")
            .Select(p=>Guard.PathName(Path.GetRelativePath(input,p))).OrderBy(p=>p,StringComparer.Ordinal).ToArray();
        if(launchers.Length>16)throw new FormatException("A Binary archive exceeds the bounded launcher-selection limit (16).");
        if(launchers.Length==0){
            if(files.Length!=1)throw new FormatException("A Binary archive must contain Underground2 User-mode launchers or one standalone VERSN2 data script.");
            Launcher=Guard.PathName(Path.GetRelativePath(input,files[0]));if(requested!=null&&Launcher!=requested)throw new FormatException("Binary launcher changed.");Used.Add(Launcher);Name=Path.GetFileNameWithoutExtension(Launcher);Script(Launcher,0);
            if(Copies.Count==0&&dataEdits==0)throw new FormatException("The Binary script contains no supported data edits or runtime resources.");return;
        }
        if(requested!=null){
            if(!launchers.Contains(requested,StringComparer.Ordinal)||!UserLauncher(requested))throw new FormatException("The selected Binary launcher is missing or is not an Underground2 User-mode installer.");
            ReadLauncher(requested);return;
        }
        var compatible=new List<string>();var failures=new List<string>();
        foreach(string candidate in launchers){
            try{
                if(!UserLauncher(candidate)){failures.Add(candidate+": not an Underground2 User-mode installer");continue;}
                // Each candidate gets independent typed database caches. A failed
                // alternate must never mutate the selected candidate's state.
                var probeWorkspace=new Workspace{Base=workspace.Base,Output=workspace.Output,Scratch=workspace.Scratch};
                foreach(string copied in workspace.Copied)probeWorkspace.Copied.Add(copied);
                new InstallPlanner(input,probeWorkspace,false).ReadLauncher(candidate);
                compatible.Add(candidate);
            }catch(Exception e) when(e is FormatException||e is NotSupportedException||e is JsonException||e is IOException||e is InvalidOperationException||e is ArgumentException){
                string message=e.Message.Replace('|',' ').Replace('\r',' ').Replace('\n',' ');if(message.Length>220)message=message.Substring(0,220);failures.Add(candidate+": "+message);
            }
        }
        if(compatible.Count==0)throw new FormatException("No compatible Underground2 User-mode Binary installer was found. "+string.Join("; ",failures));
        if(compatible.Count>1)throw new LauncherSelectionException(compatible);
        ReadLauncher(compatible[0]);
        if(launchers.Length>1){Notes.Add("Selected compatible Binary entry: "+Launcher);foreach(string failure in failures)Notes.Add("Other entry: "+failure);}
    }
    void ReadLauncher(string path){
        Launcher=path;Used.Add(Launcher);
        using var json=LauncherJson(Launcher);var root=json.RootElement;
        if(root.GetProperty("Usage").GetString()!="User"||root.GetProperty("Game").GetString()!="Underground2")throw new NotSupportedException("Only Underground2 User-mode Binary data installers are supported.");
        string dir=Path.GetDirectoryName(Launcher)?.Replace('\\','/')??"";string end=Source(dir,root.GetProperty("Endscript").GetString());Name=Path.GetFileName(dir);if(string.IsNullOrWhiteSpace(Name))Name="Binary mod";
        Script(end,0);if(Copies.Count==0&&dataEdits==0)throw new FormatException("The Binary installer contains no supported data edits or runtime resources.");
    }
    static List<string> Tokens(string line){
        var words=new List<string>();var text=new StringBuilder();bool quote=false,token=false;
        for(int p=0;p<line.Length;++p){char c=line[p];if(!quote&&c=='/'&&p+1<line.Length&&line[p+1]=='/')break;
            if(c=='"'){quote=!quote;token=true;continue;}if(!quote&&char.IsWhiteSpace(c)){if(token){words.Add(text.ToString());text.Clear();token=false;}}else{text.Append(c);token=true;}}
        if(quote)throw new FormatException("Unterminated installer string.");if(token)words.Add(text.ToString());return words;
    }
    void Script(string path,int depth){
        if(depth>12||!visiting.Add(path))throw new FormatException("Installer include cycle/depth exceeded.");
        string[] lines=Encoding.UTF8.GetString(Guard.Read(Guard.Within(input,path),262144)).TrimStart('\uFEFF').Replace("\r","").Split('\n');
        if(lines.Length>4096||lines[0].Trim()!="[VERSN2]")throw new FormatException("Expected a bounded VERSN2 data script.");
        string dir=Path.GetDirectoryName(path)?.Replace('\\','/')??"";int pos=1;string stop=Parse(lines,ref pos,true,dir,depth,0);if(stop!=""||pos<lines.Length)throw new FormatException("Unmatched installer block.");visiting.Remove(path);
    }
    // Block grammar is interpreted into a bounded set of typed data edits. No
    // Endscript executor, reflection calls, command shell or bundled code runs.
    string Parse(string[] lines,ref int pos,bool active,string dir,int depth,int nesting){
        if(nesting>16)throw new FormatException("Installer block depth exceeded.");
        while(pos<lines.Length){int lineNo=pos+1;string line=lines[pos++];if(line.Length>4096)throw new FormatException("Installer line is too long.");var a=Tokens(line);if(a.Count==0)continue;
            string op=a[0].ToLowerInvariant();if(op=="end"||op=="else")return op;if(op=="do"||op=="{"||op=="}")continue;
            if(++commands>8192)throw new FormatException("Installer command budget exceeded.");
            if(op=="if"){
                bool yes=false;if(a.Count==4&&a[1]=="file_exists"&&a[2]=="absolute")yes=workspace.Exists(Guard.PathName(a[3]));
                else if(a.Count==5&&a[1]=="collection_exists")yes=workspace.Collection(Guard.PathName(a[2]),a[3],a[4]);
                else throw new NotSupportedException("Unsupported Binary condition at line "+lineNo);
                string stop=Parse(lines,ref pos,active&&yes,dir,depth,nesting+1);if(stop=="else")stop=Parse(lines,ref pos,active&&!yes,dir,depth,nesting+1);if(stop!="end")throw new FormatException("Unterminated installer condition.");continue;
            }
            if(op=="combobox"){
                if(a.Count<3||a.Count>18)throw new FormatException("Invalid Binary choice list.");string first=a[1];if(first.IndexOf("uninstall",StringComparison.OrdinalIgnoreCase)>=0)throw new NotSupportedException("Installer's default action is uninstall; choose a supported installation package.");
                if(active)Notes.Add("Default choice: "+first);bool selected=false;int level=0;
                while(pos<lines.Length){var label=Tokens(lines[pos]);if(label.Count==0){++pos;continue;}if(label[0]=="end"&&level==0){++pos;break;}
                    if(label.Count==1&&a.Take(a.Count-1).Skip(1).Contains(label[0])){++pos;bool match=label[0]==first;selected|=match;
                        var branch=new List<string>{"[VERSN2]"};int inner=0;
                        while(pos<lines.Length){var n=Tokens(lines[pos]);if(inner==0&&n.Count==1&&(n[0]=="end"||a.Take(a.Count-1).Skip(1).Contains(n[0])))break;if(n.Count>0&&(n[0]=="if"||n[0]=="combobox"))++inner;if(n.Count>0&&n[0]=="end")--inner;branch.Add(lines[pos++]);}
                        int p=1;Parse(branch.ToArray(),ref p,active&&match,dir,depth,nesting+1);
                    }else throw new FormatException("Invalid installer choice branch.");
                }if(!selected)throw new FormatException("Installer default choice has no branch.");continue;
            }
            if(!active)continue;
            try{
                if(op=="append"&&a.Count==2){Script(Source(dir,a[1]),depth+1);continue;}
                if((op=="move_file"||op=="copy_file")&&a.Count==6&&a[1]=="override"&&a[2]=="relative"&&a[3]=="absolute"){
                    string src=Source(dir,a[4]),dest=Guard.PathName(a[5]);if(!(dest.StartsWith("cars/")||dest.StartsWith("scripts/unlimiterdata/"))||!(dest.EndsWith(".bin")||dest.EndsWith(".ini")))throw new NotSupportedException("Binary file target needs another data adapter: "+dest);
                    if(Copies.Any(c=>c[1]==dest))throw new FormatException("Duplicate installer output: "+dest);Copies.Add(new[]{src,dest});workspace.Copied.Add(dest);continue;
                }
                if(op=="import"&&a.Count==5&&a[1]=="override"){string src=Source(dir,a[4]),db=Guard.PathName(a[2]);workspace.Data(db).Import(a[3],Guard.Within(input,src));++dataEdits;continue;}
                if(op=="copy_incareer"&&a.Count==7&&a[3]=="GCareers"&&a[4]=="Main")throw new NotSupportedException("Malformed copy_incareer argument count.");
                if(op=="copy_incareer"&&a.Count==7&&a[2]=="GCareers"&&a[3]=="Main"&&a[4]=="GCarUnlocks"){workspace.Data(Guard.PathName(a[1])).Unlock(a[5],a[6]);++dataEdits;continue;}
                if(op=="update_collection"&&a.Count==6&&a[4]=="CollectionName"){workspace.Data(Guard.PathName(a[1])).Rename(a[2],a[3],a[5]);++dataEdits;continue;}
                if(op=="add_or_replace_texture"&&a.Count==8&&a[2]=="TPKBlocks"&&a[4]==a[5])throw new NotSupportedException("Malformed texture command.");
                if(op=="add_or_replace_texture"&&a.Count==7&&a[2]=="TPKBlocks"&&a[4]==a[5]){string src=Source(dir,a[6]);workspace.Data(Guard.PathName(a[1])).Texture(a[3],a[4],Guard.Within(input,src));++dataEdits;continue;}
                if(op=="replace_texture"&&a.Count==6&&a[2]=="TPKBlocks"){string src=Source(dir,a[5]);workspace.Data(Guard.PathName(a[1])).ReplaceTexture(a[3],a[4],Guard.Within(input,src));++dataEdits;continue;}
                if(op=="add_or_update_string"&&a.Count==8&&a[2]=="STRBlocks"&&a[4]=="AUTO")throw new NotSupportedException("Malformed language command.");
                if(op=="add_or_update_string"&&a.Count==7&&a[2]=="STRBlocks"&&a[4]=="AUTO"){workspace.Data(Guard.PathName(a[1])).Text(a[3],a[5],a[6]);++dataEdits;continue;}
                if(op=="new"&&a.Count==3&&a[1]=="negate"){if(!workspace.Exists(Guard.PathName(a[2])))throw new FormatException("Installer database is absent.");continue;}
                if(op=="delete"&&a.Count==2){Guard.PathName(a[1]);continue;} // Binary unload/save, never filesystem deletion.
                if(op=="unlock_memory"&&a.Count==2&&a[1]=="all"){if(cook)workspace.Memory();++dataEdits;continue;}
                if(op=="speedreflect"&&a.Count==2&&a[1]=="auto"){Required.Add("nfsu2-unlimiter|3.1.1.1337");Notes.Add("Addon runtime: Undercoop / verified Unlimiter car compatibility.");continue;}
                throw new NotSupportedException("Binary operation needs a data adapter: "+op);
            }catch(Exception e){throw new FormatException("Installer line "+lineNo+": "+e.Message,e);}
        }return "";
    }
}
internal sealed class Workspace {
    internal string Base,Output,Scratch;internal readonly HashSet<string> Copied=new HashSet<string>();readonly Dictionary<string,DataFile> data=new Dictionary<string,DataFile>();
    internal bool Exists(string path)=>Copied.Contains(path)||File.Exists(Guard.Within(Output,path))||File.Exists(Guard.Within(Base,path));
    internal byte[] Bytes(string path){string p=Guard.Within(Output,path);return Guard.Read(File.Exists(p)?p:Guard.Within(Base,path));}
    internal DataFile Data(string path){if(!data.TryGetValue(path,out var file)){file=new DataFile(path,Bytes(path),Scratch);data.Add(path,file);}return file;}
    internal bool Collection(string path,string manager,string name)=>Exists(path)&&Data(path).Has(manager,name);
    internal void Memory(){foreach(string n in new[]{"carheadersmemoryfile","frontendmemoryfile","ingamememoryfile","permanentmemoryfile","globalmemoryfile"}){string p="global/"+n+".bin";if(!Exists(p))continue;string target=Guard.Within(Output,p);Directory.CreateDirectory(Path.GetDirectoryName(target));if(File.Exists(target))throw new FormatException("Two installers request conflicting memory outputs.");if(n=="globalmemoryfile")Nikki.Utils.MemoryUnlock.LongUnlock(Create(target,Bytes(p)));else Nikki.Utils.MemoryUnlock.FastUnlock(Create(target,Bytes(p)));}}
    static string Create(string p,byte[] b){using(var f=new FileStream(p,FileMode.CreateNew,FileAccess.Write)){f.Write(b,0,b.Length);}return p;}
    internal void Finish(){foreach(var pair in data){byte[] bytes=pair.Value.Finish();if(!bytes.SequenceEqual(Interop.Decompress(Bytes(pair.Key))))Guard.New(Output,pair.Key,bytes);}}
}
}
