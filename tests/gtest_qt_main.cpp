#include <QCoreApplication>

#include <gtest/gtest.h>

// Qt parents single-shot timers to the thread's event dispatcher, which only a
// QCoreApplication provides; without one, every QTimer::singleShot left
// pending by the code under test is an allocation nobody frees.
int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    QCoreApplication app(argc, argv);
    return RUN_ALL_TESTS();
}
