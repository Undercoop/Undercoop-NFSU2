// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Text;
using System.Security.Cryptography;
using Nikki.Core;
using Nikki.Utils;
using Nikki.Reflection.Enum;
using Nikki.Support.Underground2;
using Nikki.Support.Underground2.Attributes;

namespace Undercoop.Binary {
internal static class Guard {
    internal const int FileLimit=64*1024*1024;
    internal static string PathName(string value){
        if(value==null||value.Length==0||value.Length>220)throw new FormatException("Invalid data path.");
        value=value.Replace('\\','/').ToLowerInvariant();
        foreach(char c in value)if(!(c>='a'&&c<='z'||c>='0'&&c<='9'||c=='_'||c=='-'||c=='.'||c=='/'))throw new FormatException("Unsupported data path: "+value);
        foreach(string p in value.Split('/')){string n=p.Split('.')[0];if(p.Length==0||p=="."||p==".."||p[0]=='.'||p.EndsWith(".")||new[]{"con","prn","aux","nul","clock$"}.Contains(n)||n.Length==4&&(n.StartsWith("com")||n.StartsWith("lpt"))&&n[3]>='1'&&n[3]<='9')throw new FormatException("Unsafe data path.");}
        return value;
    }
    internal static string Within(string root,string relative){
        string path=System.IO.Path.Combine(root,PathName(relative).Replace('/',System.IO.Path.DirectorySeparatorChar));
        string parent=root;foreach(string part in PathName(relative).Split('/')){parent=System.IO.Path.Combine(parent,part);if(File.Exists(parent)||Directory.Exists(parent))if((File.GetAttributes(parent)&FileAttributes.ReparsePoint)!=0)throw new FormatException("Linked data paths are not supported.");}
        return path;
    }
    internal static byte[] Read(string path,int limit=FileLimit){var info=new FileInfo(path);if(!info.Exists||info.Length>limit||(info.Attributes&FileAttributes.ReparsePoint)!=0)throw new FormatException("Missing, linked or oversized input resource.");return File.ReadAllBytes(path);}
    internal static void New(string root,string relative,byte[] value){if(value.Length>FileLimit)throw new FormatException("Generated resource is too large.");string path=Within(root,relative);Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path));using var stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.Read);stream.Write(value,0,value.Length);stream.Flush(true);}
    internal static string Sha(byte[] b)=>Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();
    internal static uint U(byte[] b,int p){if(p<0||p>b.Length-4)throw new FormatException("Truncated native chunk.");return BitConverter.ToUInt32(b,p);}
    internal static void W(byte[] b,int p,uint value){if(p<0||p>b.Length-4)throw new FormatException("Invalid native write.");Buffer.BlockCopy(BitConverter.GetBytes(value),0,b,p,4);}
}
internal sealed class Chunk {
    internal uint Id;internal byte[] Bytes;internal int Residue;
    internal static List<Chunk> Read(byte[] data){
        var list=new List<Chunk>();int p=0;
        while(p<data.Length){if(list.Count>=65536||data.Length-p<8)throw new FormatException("Invalid native chunk stream.");uint id=Guard.U(data,p),n=Guard.U(data,p+4);if(n>data.Length-p-8)throw new FormatException("Native chunk exceeds its resource.");var b=new byte[(int)n+8];Buffer.BlockCopy(data,p,b,0,b.Length);list.Add(new Chunk{Id=id,Bytes=b,Residue=p&127});p+=b.Length;}return list;
    }
    internal static byte[] Join(IEnumerable<Chunk> chunks){using var ms=new MemoryStream();using var bw=new BinaryWriter(ms);
        foreach(var c in chunks){if(c.Id!=0&&(ms.Position&127)!=c.Residue){int n=(c.Residue-(int)ms.Position-8)&127;bw.Write(0);bw.Write(n);bw.Write(new byte[n]);}bw.Write(c.Bytes);if(ms.Length>Guard.FileLimit)throw new FormatException("Native resource budget exceeded.");}return ms.ToArray();}
}
internal sealed class DataFile {
    internal readonly string Name;readonly string scratch;internal Datamap Map;readonly byte[] original;
    readonly List<Chunk> originalChunks;readonly HashSet<uint> changed=new HashSet<uint>();
    readonly Dictionary<string,byte[]> preserved=new Dictionary<string,byte[]>();
    internal DataFile(string name,byte[] bytes,string scratchRoot){
        Name=name;scratch=scratchRoot;original=Interop.Decompress(bytes);originalChunks=Chunk.Read(original);Map=new Datamap();
        var selected=originalChunks.Where(c=>c.Id==0x34600||c.Id==0x34601||c.Id==0x80034602||c.Id==0xB3300000||c.Id==0x39000).ToArray();
        string input=System.IO.Path.Combine(scratch,Guid.NewGuid().ToString("N")+".bin");File.WriteAllBytes(input,Chunk.Join(selected));Map.Load(new Options(input,"Undercoop data engine 1"));NormalizePartKeys();
        foreach(var model in Map.DBModelParts)preserved["parts/"+model.CollectionName]=Export("DBModelParts",model.CollectionName);
        foreach(var block in Map.STRBlocks)preserved["strings/"+block.CollectionName]=Export("STRBlocks",block.CollectionName);
        foreach(var block in Map.TPKBlocks)preserved["textures/"+block.CollectionName]=Export("TPKBlocks",block.CollectionName);
    }
    // Nikki's global display-name lookup learns keys while reading/saving.
    // A resolved name and its 0xXXXXXXXX spelling are the same native value,
    // but serialize to different text. Canonical numeric spellings keep strict
    // before/after collection comparison stable without discarding any key.
    void NormalizePartKeys(){
        foreach(var model in Map.DBModelParts)foreach(var part in model.ModelCarParts)foreach(var attribute in part.Attributes){
            if(attribute is KeyAttribute key)key.Value="0x"+key.Value.BinHash().ToString("X8");
            else if(attribute is CustomAttribute custom&&custom.Type==CarPartAttribType.Key)custom.ValueKey="0x"+custom.ValueKey.BinHash().ToString("X8");
        }
    }
    byte[] Export(string manager,string name){using var ms=new MemoryStream();using var bw=new BinaryWriter(ms);Map.Export(manager,name,bw,true);return ms.ToArray();}
    internal bool Has(string manager,string name){var m=Map[manager];return m!=null&&m.FindIndex(name)>=0;}
    internal void Import(string manager,string file){
        if(manager!="CarTypeInfos"&&manager!="DBModelParts"&&manager!="TPKBlocks"&&manager!="STRBlocks")throw new NotSupportedException("Binary import manager needs an adapter: "+manager);
        int before=Map.CarTypeInfos.Count;var beforeNames=Map.CarTypeInfos.Select(c=>c.CollectionName).ToArray();
        var originalCars=manager=="CarTypeInfos"?beforeNames.ToDictionary(n=>n,n=>Export("CarTypeInfos",n)):null;
        using var br=new BinaryReader(new MemoryStream(Guard.Read(file,8*1024*1024)));Map.Import(SerializeType.Override,manager,br);if(manager=="DBModelParts")NormalizePartKeys();
        if(br.BaseStream.Position!=br.BaseStream.Length)throw new FormatException("Trailing serialized collection data.");
        if(manager=="CarTypeInfos"){
            if(Map.CarTypeInfos.Count<before||Map.CarTypeInfos.Count>256)throw new FormatException("Unsupported addon catalog size.");
            for(int i=0;i<before;++i){
                if(Map.CarTypeInfos[i].CollectionName!=beforeNames[i])throw new FormatException("Addon changed existing model order.");
                // The append-only adapter later restores original native rows
                // verbatim. Refuse a requested replacement now rather than
                // silently discarding that intentional database modification.
                if(!originalCars[beforeNames[i]].SequenceEqual(Export("CarTypeInfos",beforeNames[i])))throw new NotSupportedException("Binary replacement of an existing car record needs an explicit adapter: "+beforeNames[i]);
            }
            changed.Add(0x34600);changed.Add(0x34601);
        }else if(manager=="DBModelParts"){changed.Add(0x80034602);foreach(var model in Map.DBModelParts){string key="parts/"+model.CollectionName;if(preserved.ContainsKey(key)&&!preserved[key].SequenceEqual(Export(manager,model.CollectionName)))throw new FormatException("Addon tries to replace an existing model's parts: "+model.CollectionName);}}
        else {changed.Add(manager=="TPKBlocks"?0xB3300000u:0x39000u);}
    }
    internal void Rename(string manager,string from,string to){if(manager!="TPKBlocks")throw new NotSupportedException("Unsupported collection rename.");var b=Map.TPKBlocks[from];if(b==null)throw new FormatException("Missing texture collection.");preserved.Remove("textures/"+from);b.CollectionName=to;changed.Add(0xB3300000);}
    internal void Texture(string collection,string name,string dds){var b=Map.TPKBlocks[collection];if(b==null)throw new FormatException("Texture collection is missing: "+collection);preserved.Remove("textures/"+collection);if(b.FindTexture(name.BinHash(),KeyType.BINKEY)==null)b.AddTexture(name,dds);else b.ReplaceTexture(name.BinHash(),KeyType.BINKEY,dds);changed.Add(0xB3300000);}
    internal void ReplaceTexture(string collection,string name,string dds){
        var b=Map.TPKBlocks[collection];if(b==null)throw new FormatException("Texture collection is missing: "+collection);
        if(b.FindTexture(name.BinHash(),KeyType.BINKEY)==null)throw new FormatException("Replacement texture is missing: "+collection+"/"+name);
        b.ReplaceTexture(name.BinHash(),KeyType.BINKEY,dds);preserved.Remove("textures/"+collection);changed.Add(0xB3300000);
    }
    internal void Text(string collection,string key,string text){var b=Map.STRBlocks[collection];if(b==null)throw new FormatException("Language collection is missing: "+collection);preserved.Remove("strings/"+collection);var r=b.GetRecord(key.BinHash());if(r!=null)r.Text=text;else b.AddRecord("AUTO",key,text);changed.Add(0x39000);}
    internal void Unlock(string from,string to){
        uint a=from.BinHash(),b=to.BinHash();int hits=0;
        foreach(var c in originalChunks.Where(c=>c.Id==0x80034A10)){
            var children=Chunk.Read(c.Bytes.Skip(8).ToArray());foreach(var row in children.Where(n=>n.Id==0x34A1F)){
                int bytes=row.Bytes.Length-8;if(bytes%12!=0)throw new FormatException("Unsupported career car unlock layout.");
                for(int p=8;p<row.Bytes.Length;p+=12)if(Guard.U(row.Bytes,p)==b)throw new FormatException("Career unlock already exists for addon: "+to);
                for(int p=8;p<row.Bytes.Length;p+=12)if(Guard.U(row.Bytes,p)==a){byte[] replacement=new byte[row.Bytes.Length+12];Buffer.BlockCopy(row.Bytes,0,replacement,0,row.Bytes.Length);Buffer.BlockCopy(row.Bytes,p,replacement,row.Bytes.Length,12);Guard.W(replacement,row.Bytes.Length,b);Guard.W(replacement,4,(uint)(replacement.Length-8));row.Bytes=replacement;++hits;break;}
            }
            byte[] body=Chunk.Join(children);var next=new byte[body.Length+8];Guard.W(next,0,c.Id);Guard.W(next,4,(uint)body.Length);Buffer.BlockCopy(body,0,next,8,body.Length);c.Bytes=next;
        }
        if(hits!=1)throw new FormatException("The source career car unlock must be unique: "+from);changed.Add(0x80034A10);
    }
    internal byte[] Finish(){
        if(changed.Count==0)return original;
        string output=System.IO.Path.Combine(scratch,Guid.NewGuid().ToString("N")+".bin");Map.Save(new Options(output,"Undercoop data engine 1"));var generated=Chunk.Read(Guard.Read(output));
        // Existing car records/skins are copied verbatim; only append newly imported
        // rows with their stable new native indices from the typed assembler.
        var carOld=originalChunks.SingleOrDefault(c=>c.Id==0x34600);var carNew=generated.SingleOrDefault(c=>c.Id==0x34600);
        if(changed.Contains(0x34600)&&carOld!=null&&carNew!=null){int oldStart=(16-carOld.Residue%16)%16;if(oldStart<8)oldStart+=16;int newStart=(16-carNew.Residue%16)%16;if(newStart<8)newStart+=16;int n=carOld.Bytes.Length-oldStart;if(n%0x890!=0||carNew.Bytes.Length-newStart<n)throw new FormatException("Cannot preserve original car records.");Buffer.BlockCopy(carOld.Bytes,oldStart,carNew.Bytes,newStart,n);}
        var outputChunks=new List<Chunk>();var inserted=new HashSet<uint>();
        foreach(var c in originalChunks){
            if(!changed.Contains(c.Id)||c.Id==0x80034A10){outputChunks.Add(c);continue;}
            if(!inserted.Add(c.Id)){
                // Texture/language managers contain one native chunk per named
                // collection. Their complete generated group was placed at the
                // first original slot; later old members must not be duplicated.
                if(c.Id==0xB3300000||c.Id==0x39000)continue;
                throw new FormatException("Ambiguous native data block.");
            }
            var replacements=generated.Where(n=>n.Id==c.Id).ToArray();if(replacements.Length==0)throw new FormatException("Generated data block is missing.");outputChunks.AddRange(replacements);
        }
        foreach(uint id in changed)if(id!=0x80034A10&&!inserted.Contains(id)){var a=generated.Where(c=>c.Id==id).ToArray();if(a.Length==0)throw new FormatException("Generated data block is missing.");outputChunks.AddRange(a);}
        byte[] result=Chunk.Join(outputChunks);
        // Re-read only touched library data, then require all untouched typed
        // collections to retain identical serialized values (including raw keys).
        var verify=new DataFile(Name,result,scratch);foreach(var pair in preserved){string[] key=pair.Key.Split('/');string manager=key[0]=="parts"?"DBModelParts":key[0]=="strings"?"STRBlocks":"TPKBlocks";if(!pair.Value.SequenceEqual(verify.Export(manager,key[1])))throw new FormatException("Round-trip verification failed for original "+pair.Key);}
        return result;
    }
}
}
