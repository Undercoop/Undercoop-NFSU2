PROG = 7z.dll
# USE_C_LZFINDOPT = 1
DEF_FILE = UndercoopArchive.def
CFLAGS = $(CFLAGS) \
  -DZ7_EXTERNAL_CODECS \
  -DUNDERCOOP_ARCHIVE_BUDGET=1 \

!IFNDEF UNDER_CE
!ENDIF

!include "Arc.mak"

COMMON_OBJS = $(COMMON_OBJS) \
  $O\UndercoopBudgetCpp.obj \

C_OBJS = $(C_OBJS) \
  $O\UndercoopBudget.obj \

COMPRESS_OBJS = $(COMPRESS_OBJS) \
  $O\CodecExports.obj \

AR_OBJS = $(AR_OBJS) \
  $O\ArchiveExports.obj \
  $O\DllExports2.obj \


!include "../../7zip.mak"
