#include "optionstreewriter.h"

#include "optionstree.h"
#include "varianttree.h"

#include <QBuffer>
#include <QKeySequence>
#include <QRect>
#include <QSize>

OptionsTreeWriter::OptionsTreeWriter(const OptionsTree *options) : options_(options) { Q_ASSERT(options_); }

void OptionsTreeWriter::setName(const QString &configName) { configName_ = configName; }

void OptionsTreeWriter::setNameSpace(const QString &configNS) { configNS_ = configNS; }

void OptionsTreeWriter::setVersion(const QString &configVersion) { configVersion_ = configVersion; }

bool OptionsTreeWriter::write(QIODevice *device)
{
    setDevice(device);

    // turn it off for even more speed
    setAutoFormatting(true);
    setAutoFormattingIndent(1);

    writeStartDocument();
    writeDTD(QString("<!DOCTYPE %1>").arg(configName_));
    writeStartElement(configName_);
    writeAttribute("version", configVersion_);
    writeAttribute("xmlns", configNS_);

    writeTree(&options_->tree_);

    writeEndDocument();
    return true;
}

void OptionsTreeWriter::writeTree(const VariantTree *tree)
{
    const auto &nodes = tree->trees_.keys();
    for (const QString &node : nodes) {
        Q_ASSERT(!node.isEmpty());
        writeStartElement(node);
        if (tree->comments_.contains(node))
            writeAttribute("comment", tree->comments_[node]);

        writeTree(tree->trees_[node]);
        writeEndElement();
    }

    const auto &children = tree->values_.keys();
    for (const QString &child : children) {
        Q_ASSERT(!child.isEmpty());
        writeStartElement(child);
        if (tree->comments_.contains(child))
            writeAttribute("comment", tree->comments_[child]);

        writeVariant(tree->values_[child]);
        writeEndElement();
    }

    const auto &unknowns = tree->unknowns2_.keys();
    for (const QString &unknown : unknowns) {
        writeUnknown(tree->unknowns2_[unknown]);
    }
}

void OptionsTreeWriter::writeVariant(const QVariant &variant)
{
    writeAttribute("type", variant.typeName());
    if (variant.type() == QVariant::StringList) {
        const auto &sList = variant.toStringList();
        for (const QString &s : sList) {
            writeStartElement("item");
            writeCharacters(s);
            writeEndElement();
        }
    } else if (variant.type() == QVariant::List) {
        const auto &variants = variant.toList();
        for (const QVariant &v : variants) {
            writeStartElement("item");
            writeVariant(v);
            writeEndElement();
        }
    } else if (variant.type() == QVariant::Size) {
        writeTextElement("width", QString::number(variant.toSize().width()));
        writeTextElement("height", QString::number(variant.toSize().height()));
    } else if (variant.type() == QVariant::Rect) {
        writeTextElement("x", QString::number(variant.toRect().x()));
        writeTextElement("y", QString::number(variant.toRect().y()));
        writeTextElement("width", QString::number(variant.toRect().width()));
        writeTextElement("height", QString::number(variant.toRect().height()));
    } else if (variant.type() == QVariant::ByteArray) {
        writeCharacters(variant.toByteArray().toBase64());
    } else if (variant.type() == QVariant::KeySequence) {
        QKeySequence k = variant.value<QKeySequence>();
        writeCharacters(k.toString());
    } else {
        writeCharacters(variant.toString());
    }
}

void OptionsTreeWriter::writeUnknown(const QString &unknown)
{
    QByteArray ba = unknown.toUtf8();
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::ReadOnly);
    QXmlStreamReader reader;
    reader.setDevice(&buffer);

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            readUnknownTree(&reader);
        }
    }
}

void OptionsTreeWriter::readUnknownTree(QXmlStreamReader *reader)
{
    Q_ASSERT(reader->isStartElement());
    writeStartElement(reader->name().toString());
    const auto &attrs = reader->attributes();
    for (const QXmlStreamAttribute &attr : attrs) {
        writeAttribute(attr.name().toString(), attr.value().toString());
    }

    while (!reader->atEnd()) {
        writeCharacters(reader->text().toString());
        reader->readNext();

        if (reader->isEndElement())
            break;

        if (reader->isStartElement())
            readUnknownTree(reader);
    }

    writeEndElement();
}
