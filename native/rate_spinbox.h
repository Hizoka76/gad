#pragma once
// Sept décimales en Mio/s conservent chaque entier d’octets/s jusqu’à la limite de 1 Tio/s.
#include <QDoubleSpinBox>
#include "i18n.h"

class SpeedLimitSpinBox : public QDoubleSpinBox {
public:
    explicit SpeedLimitSpinBox(QWidget *parent=nullptr):QDoubleSpinBox(parent) {
        setDecimals(7);setRange(0,1048576);setKeyboardTracking(false);
        I18n::onChange(this,"numberLocale",[this]{setLocale(QLocale());});
    }
    /// Même précision pour les descriptions de profils, sans construire de widget.
    static QString rateText(qint64 bytes,const QLocale &locale=QLocale()) {
        return compact(locale.toString(bytes/1048576.0,'f',7),locale);
    }
protected:
    /// L’édition interactive choisit au moins un octet/s pour toute valeur positive.
    double valueFromText(const QString &text) const override {
        const auto value=QDoubleSpinBox::valueFromText(text);
        return value>0?qMax<qint64>(1,qRound64(value*1048576))/1048576.0:0.;
    }
    QString textFromValue(double value) const override {
        return compact(QDoubleSpinBox::textFromValue(value),locale());
    }
private:
    /// Garder deux décimales usuelles et uniquement les chiffres nécessaires pour les petites limites.
    static QString compact(QString text,const QLocale &locale) {
        const auto separator=locale.decimalPoint(),zero=locale.zeroDigit();
        const auto position=text.indexOf(separator);
        if(position<0 || zero.isEmpty())return text;
        const auto minimum=position+separator.size()+2*zero.size();
        while(text.size()>minimum && text.endsWith(zero))text.chop(zero.size());
        return text;
    }
};
