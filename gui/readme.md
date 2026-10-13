#1. Connect to the USB/IP server on port 3240.
#2. Send an Import Request (OP_REQ_IMPORT) specifying the Bus ID of the target USB device.
#3. Receive the Server's Reply (OP_REP_IMPORT) confirming the connection is locked.
#4. Enter a Loop Sending USBIP_CMD_SUBMIT packets to request data from a specific USB input endpoint (IN direction), then parse the incoming USBIP_RET_SUBMIT packets containing your raw data stream.
