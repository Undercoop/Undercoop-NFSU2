using System;
using System.IO;
using System.Collections.Generic;
using Nikki.Support.Shared.Parts.CarParts;
using UG2Struct=Nikki.Support.Underground2.Parts.CarParts.CPStruct;

namespace Nikki.Utils {
// Native table indices must depend on complete data equality. A 32-bit
// GetHashCode value is only a dictionary bucket, never proof of equality.
internal static class SafePartsKeys {
    static readonly Dictionary<string,int> values=new Dictionary<string,int>(StringComparer.Ordinal);
    static readonly Dictionary<object,int> references=new Dictionary<object,int>(ReferenceEqualityComparer.Instance);
    static int next;
    internal static void Reset(){values.Clear();references.Clear();next=0;}
    static int Intern(string key){if(values.TryGetValue(key,out int value))return value;if(next>=1000000)throw new FormatException("Car-part interning budget exceeded.");values.Add(key,++next);return next;}
    internal static int String(string value)=>Intern("s|"+(value??System.String.Empty));
    internal static int Real(object value){if(references.TryGetValue(value,out int id))return id;if(next>=1000000)throw new FormatException("Car-part reference budget exceeded.");references.Add(value,++next);return next;}
    internal static int Attribute(CPAttribute value){using var ms=new MemoryStream();using var writer=new BinaryWriter(ms);value.Serialize(writer);return Intern("a|"+value.GetType().FullName+"|"+Convert.ToBase64String(ms.ToArray()));}
    internal static int Structure(UG2Struct value){using var ms=new MemoryStream();using var writer=new BinaryWriter(ms);value.Serialize(writer);return Intern("g|"+Convert.ToBase64String(ms.ToArray()));}
    internal static int Offset(IEnumerable<ushort> offsets){using var ms=new MemoryStream();using var writer=new BinaryWriter(ms);foreach(ushort value in offsets)writer.Write(value);return Intern("o|"+Convert.ToBase64String(ms.ToArray()));}
}
}
