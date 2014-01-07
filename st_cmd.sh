#! /bin/sh


gattt="/sensortag/bluez-5.12/attrib/gatttool"

# MAC address - not hardcoded anymore but read as parameter
#st1="90:59:AF:0B:81:2C"

st1="$1"
#echo "Device used : "$st1


hciconfig hci0 down
hciconfig hci0 up
hciconfig hci0 name daisypi
hciconfig hci0 piscan



# ENABLING ALL THE NEEDED SENSORS
#-----------------------------------------------
#enable IR sensor
$gattt -b $st1 --char-write --handle=0x0029 --value 01
#enable SHT21 - temp /humidity
$gattt -b $st1 --char-write --handle=0x003C --value 01
sleep 1

# SHT21 temp + humidity
#--------------------------------------------------
hum_string=$($gattt -b $st1 --char-read --handle=0x0038)
hum_number=$(echo $hum_string | tr '[:lower:]' '[:upper:]')
pre1=$(echo $hum_number | awk '{print $4 $3}')
dec1=$(echo "ibase=16; $pre1"|bc)
pre1=$(echo $hum_number | awk '{print $6 $5}')
dec2=$(echo "ibase=16; $pre1"|bc)

#FORMULA for temperature Temp = 172.72 * decimal(4,3) / 65536 - 46.86

fin11=$(echo "scale=6; $dec1/65536"|bc)
fin12=$(echo "scale=6; $fin11*172.72"|bc)
t_sht=$(echo "scale=6; $fin12-48.86"|bc)
#echo "Temp SHT21  : "$fin13
#FORMULA for humidity   Hum = 125 * decimal(6,5) / 65536 - 6
fin11=$(echo "scale=6; $dec2/65536"|bc)
fin12=$(echo "scale=6; $fin11*125"|bc)
h_sht=$(echo "scale=6; $fin12-6"|bc)
#echo "Humid SHT21 : "$fin13


# IR temperature - ambient and object
#---------------------------------------------------
ir_temp_string=$($gattt -b $st1 --char-read --handle=0x0025)
#echo " IR  string : "$ir_temp_string
ir_temp_number=$(echo $ir_temp_string | tr '[:lower:]' '[:upper:]')
pre1=$(echo $ir_temp_number | awk '{print $4 $3}')
dec1=$(echo "ibase=16; $pre1"|bc)
pre1=$(echo $ir_temp_number | awk '{print $6 $5}')
dec2=$(echo "ibase=16; $pre1"|bc)


t_ir=$(echo "scale=6; $dec2/128"|bc)
#echo "Temp IR ambient : "$t_ir


#disable IR sensor
$gattt -b $st1 --char-write --handle=0x0029 --value 00
#disable SHT21 - temp /humidity
$gattt -b $st1 --char-write --handle=0x003C --value 00


echo "SensorTag_Read__Temp-SHT__Humid_SHT__Temp-IR-ambient: "$t_sht" "$h_sht" "$t_ir

#-----------------------------------------------------------------
# warnings, things learned, things to be done :
# 1. hcitool lecc had to be performed once; in a strange way it seems a new connection will reset all the sensors ?! and
# first couple of readings will be blank - not used anymore
# 2. Default bluez utility for wheezy is not working. Install version 5.12 and will work fine.
# 3. Data is read via this method but each sensor has it's own interpretation of those values. Real values will be achieved through PDF formulas
# provided by manufacturers.
# 4. After enabling sensors, a delay is needed for them to perform measurments and complete data conversion.






