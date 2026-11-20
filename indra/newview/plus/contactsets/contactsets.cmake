# -*- cmake -*-
# Contact sets (plus/contactsets, from Firestorm's LGGContactSets, FSFloaterContactSetConfiguration and
# FSFloaterAddToContactSet; research/333): friends (and others) in named sets with a colour, notices and
# autoresponses of their own, and aliases. Included by plus/plus.cmake. The data is read at login
# (llstartup.cpp), the aliases come through the name cache (llavatarnamecache.cpp); the windows are
# registered from the feature's init.

list(APPEND plus_SOURCE_FILES
    plus/contactsets/contactsets.cpp
    plus/contactsets/fsfloateraddtocontactset.cpp
    plus/contactsets/fsfloatercontactsetconfiguration.cpp
    plus/contactsets/lggcontactsets.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/contactsets/contactsets.h
    plus/contactsets/fsfloateraddtocontactset.h
    plus/contactsets/fsfloatercontactsetconfiguration.h
    plus/contactsets/lggcontactsets.h
    )
