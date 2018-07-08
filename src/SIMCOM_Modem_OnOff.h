#ifndef _SIMCOM_MODEM_ONOFF_H_
#define _SIMCOM_MODEM_ONOFF_H_
/*
 */

/*!
 * \brief This class is used to switch on or off a device.
 *
 * It's a pure virtual class, so you'll have to implement a specialized
 * class.
 */
class SIMCOM_Modem_OnOff
{
public:
    virtual ~SIMCOM_Modem_OnOff() {}
    virtual void on() = 0;
    virtual void off() = 0;
    virtual bool isOn() = 0;
};

class GPRSBeeOnOff : public SIMCOM_Modem_OnOff
{
    public:
        GPRSBeeOnOff();
        void init(int8_t vcc33Pin, int8_t onoffPin, int8_t statusPin);
        void on();
        void off();
        bool isOn();
        
    
    private:
        int8_t _vcc33Pin = -1;
        int8_t _onoffPin = -1;
        int8_t _statusPin = -1;
    
};
#endif /* _SIMCOM_MODEM_ONOFF_H_ */
