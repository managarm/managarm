
.global krt_entry
krt_entry:
	.extern thorRtMain
	call thorRtMain
	hlt

