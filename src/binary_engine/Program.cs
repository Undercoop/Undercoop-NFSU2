// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;
namespace Undercoop.Binary {
internal static class Program {
    public static int Main(string[] args){
        try{
            if(args.Length!=2||(args[0]!="inspect"&&args[0]!="build"))throw new ArgumentException("Expected inspect/build and a private data workspace.");
            string root=Path.GetFullPath(args[1]).TrimEnd(Path.DirectorySeparatorChar);if(!Directory.Exists(root)||(File.GetAttributes(root)&FileAttributes.ReparsePoint)!=0)throw new FormatException("Invalid private workspace.");
            if(!Path.GetFileName(root).StartsWith("job-",StringComparison.Ordinal))throw new FormatException("Expected a new job directory.");
            Directory.SetCurrentDirectory(root);string input=Guard.Within(root,"input"),output=Guard.Within(root,"output"),basePath=Guard.Within(root,"base"),scratch=Guard.Within(root,"scratch");
            if(!Directory.Exists(input)||!Directory.Exists(output)||!Directory.Exists(basePath)||!Directory.Exists(scratch))throw new FormatException("Incomplete isolated data workspace.");
            long total=0;int count=0;var pending=new Stack<string>();pending.Push(root);while(pending.Count>0){foreach(string f in Directory.EnumerateFileSystemEntries(pending.Pop())){var attrs=File.GetAttributes(f);if((attrs&FileAttributes.ReparsePoint)!=0)throw new FormatException("Linked workspace entry.");if(++count>10000)throw new FormatException("Workspace entry budget exceeded.");if((attrs&FileAttributes.Directory)!=0)pending.Push(f);else if((total+=new FileInfo(f).Length)>768L*1024*1024)throw new FormatException("Workspace budget exceeded.");}}
            string selected=null;string selector=Guard.Within(root,"selected-launcher.txt");
            if(Directory.Exists(selector))throw new FormatException("Binary launcher selection must be a regular file.");
            if(File.Exists(selector)){selected=Encoding.UTF8.GetString(Guard.Read(selector,512)).Trim('\uFEFF',' ','\t','\r','\n');if(selected!=Guard.PathName(selected))throw new FormatException("Binary launcher selection is not canonical.");}
            var ws=new Workspace{Base=basePath,Output=output,Scratch=scratch};var planner=new InstallPlanner(input,ws,args[0]=="build");planner.Read(selected);
            if(args[0]=="build")ws.Finish();
            var lines=new List<string>{"Undercoop Binary data plan 1","launcher|"+planner.Launcher,"name|"+planner.Name};
            foreach(var c in planner.Copies)lines.Add("copy|"+c[0]+"|"+c[1]);foreach(string f in planner.Used.OrderBy(s=>s,StringComparer.Ordinal))lines.Add("raw|"+f);
            foreach(string n in planner.Notes.Distinct())lines.Add("note|"+n.Replace('|',' ').Replace('\r',' ').Replace('\n',' '));foreach(string r in planner.Required.OrderBy(s=>s,StringComparer.Ordinal))lines.Add("require|"+r);
            foreach(string f in Directory.GetFiles(output,"*",SearchOption.AllDirectories).OrderBy(s=>s,StringComparer.Ordinal)){string relative=Guard.PathName(Path.GetRelativePath(output,f));var bytes=Guard.Read(f);lines.Add("output|"+relative+"|"+Guard.Sha(bytes)+"|"+bytes.Length);}
            Guard.New(root,"plan.txt",Encoding.UTF8.GetBytes(string.Join("\n",lines)+"\n"));return 0;
        }catch(Exception e){try{if(args.Length==2){string root=Path.GetFullPath(args[1]);if(Directory.Exists(root)&&(File.GetAttributes(root)&FileAttributes.ReparsePoint)==0&&Path.GetFileName(root).StartsWith("job-",StringComparison.Ordinal)){
                if(e is LauncherSelectionException choice)Guard.New(root,"launchers.txt",Encoding.UTF8.GetBytes("Undercoop Binary launcher choices 1\n"+string.Join("\n",choice.Launchers.Select(p=>"launcher|"+p))+"\n"));
                Guard.New(root,"error.txt",Encoding.UTF8.GetBytes(e.GetType().Name+": "+e.Message));}}}catch{}return 1;}
    }
}
}
