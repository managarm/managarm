// TODO: implement this
.text
.global fastCopy16
.extern memcpy
.type fastCopy16, @function
fastCopy16:
	b memcpy
