// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Undercoop's bounded managed replacement for Nikki's native compression DLL.
// JDLZ layout follows the existing CoopCarCatalogInput.h adapter. No P/Invoke.
using System;
using Nikki.Reflection.Enum;
namespace Nikki.Utils {
public static class Interop {
    const int Limit=64*1024*1024;
    static int I(byte[] b,int p)=>BitConverter.ToInt32(b,p);
    public static byte[] Decompress(byte[] input){
        if(input==null||input.Length<8||input.Length>Limit)throw new FormatException("Invalid compressed resource size.");
        uint id=BitConverter.ToUInt32(input,0);
        if(id!=0x57574152&&id!=0x5A4C444A){
            if(id==0x46465548||id==0x504D4F43||id==0x4B504652||id==0x54534542)throw new NotSupportedException("This Binary resource uses a compression codec not supported by the managed adapter.");
            return input;
        }
        if(input.Length<16||input[4]!=(id==0x57574152?1:2)||input[5]!=16||input[6]!=0||input[7]!=0)throw new FormatException("Invalid compression header.");
        int count=I(input,8),length=I(input,12);if(count<0||count>Limit||length!=input.Length)throw new FormatException("Invalid decoded resource size.");
        var output=new byte[count];int at=16,pos=0;uint f1=1,f2=1;
        if(id==0x57574152){if(count!=input.Length-16)throw new FormatException("Invalid RAWW length.");Buffer.BlockCopy(input,16,output,0,count);return output;}
        while(pos<count){
            if(f1==1){if(at==input.Length)throw new FormatException("Truncated JDLZ flags.");f1=(uint)input[at++]|256;}
            if(f2==1){if(at==input.Length)throw new FormatException("Truncated JDLZ flags.");f2=(uint)input[at++]|256;}
            if((f1&1)!=0){
                if(at>input.Length-2)throw new FormatException("Truncated JDLZ match.");int a=input[at++],b=input[at++];
                int n=(f2&1)!=0?(b|((a&240)<<4))+3:(a&31)+3;
                int d=(f2&1)!=0?(a&15)+1:(b|((a&224)<<3))+17;f2>>=1;
                if(d>pos||n>count-pos)throw new FormatException("Invalid JDLZ reference.");
                for(int k=0;k<n;++k)output[pos+k]=output[pos+k-d];pos+=n;
            }else{if(at==input.Length)throw new FormatException("Truncated JDLZ literal.");output[pos++]=input[at++];}
            f1>>=1;
        }
        if(at!=input.Length&&(input.Length-at!=1||input[at]!=0))throw new FormatException("Trailing JDLZ data.");return output;
    }
    public static byte[] Compress(byte[] input,LZCompressionType type){
        if(input==null||input.Length>Limit)throw new FormatException("Invalid resource size.");
        // RAWW is a native recognized, lossless container. Never load an archive DLL.
        byte[] output=new byte[input.Length+16];Buffer.BlockCopy(BitConverter.GetBytes(0x57574152),0,output,0,4);output[4]=1;output[5]=16;
        Buffer.BlockCopy(BitConverter.GetBytes(input.Length),0,output,8,4);Buffer.BlockCopy(BitConverter.GetBytes(output.Length),0,output,12,4);
        Buffer.BlockCopy(input,0,output,16,input.Length);return output;
    }
    public static byte[] Compress(byte[] input,int start,int count,LZCompressionType type){
        if(input==null||start<0||count<0||count>Limit||start>input.Length-count)throw new FormatException("Invalid compression slice.");
        byte[] part=new byte[count];Buffer.BlockCopy(input,start,part,0,count);return Compress(part,type);
    }
}
}
