idf_component_register(
	SRCS 
	"main.c" 
	"wifi.c"
	EMBED_TXTFILES "TLS_Keys/ca.crt"
        
        INCLUDE_DIRS
        "Include")
