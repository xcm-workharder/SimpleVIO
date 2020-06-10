#include "initialization/initial_ex_rotation.h"
#include "utility/utility.h"
#include "parameters.h"

InitialEXRotation::InitialEXRotation() {
    frame_count = 0;
    Rc.push_back(Matrix3d::Identity());
    Rc_g.push_back(Matrix3d::Identity());
    Rimu.push_back(Matrix3d::Identity());
    ric = Matrix3d::Identity();
}

// 计算相机旋转外参,相机到IMU的旋转,平移较小,暂不考虑
bool InitialEXRotation::calibrationExRotation(vector<pair<Vector3d, Vector3d>>& corres,
                                              Quaterniond& delta_q_imu,
                                              Matrix3d& calib_ric) {
    frame_count++;
    Rc.push_back(solveRelativeR(corres)); // 计算帧间的相机旋转矩阵,由对极几何得到
    Rimu.push_back(delta_q_imu.toRotationMatrix()); // 帧间IMU旋转矩阵,预积分得出
    Rc_g.push_back(ric.inverse() * delta_q_imu * ric); //

    MatrixXd A(frame_count * 4, 4);
    A.setZero();
    int sum_ok = 0;
    for (int i = 1; i <= frame_count; ++i) {
        Quaterniond r1(Rc[i]);
        Quaterniond r2(Rc_g[i]);

        double angular_distance = 180 / M_PI * r1.angularDistance(r2); // 旋转角度
        double huber = angular_distance > 5.0 ? 5.0 / angular_distance : 1.0; // 鲁棒核函数
        ++sum_ok;
        Matrix4d L, R;

        // 利用帧间相机旋转构造左乘矩阵
        double w = Quaterniond(Rc[i]).w();
        Vector3d q = Quaterniond(Rc[i]).vec();
        L.block<3, 3>(0, 0) = w * Matrix3d::Identity() + Utility::skewSymmetric(q);
        L.block<3, 1>(0, 3) = q;
        L.block<1, 3>(3, 0) = -q.transpose();
        L(3, 3) = w;

        // 利用帧间IMU旋转矩阵构造右乘矩阵
        Quaterniond R_ij(Rimu[i]);
        w = R_ij.w();
        q = R_ij.vec();
        R.block<3, 3>(0, 0) = w * Matrix3d::Identity() - Utility::skewSymmetric(q);
        R.block<3, 1>(0, 3) = q;
        R.block<1, 3>(3, 0) = -q.transpose();
        R(3, 3) = w; // 右下角元素为四元数实部

        A.block<4, 4>((i - 1) * 4, 0) = huber * (L - R);
    }

    JacobiSVD<MatrixXd> svd(A, ComputeFullU | ComputeFullV);
    Matrix<double, 4, 1> x = svd.matrixV().col(3);
    Quaterniond estimated_R(x);
    ric = estimated_R.toRotationMatrix().inverse();
    Vector3d ric_cov;
    ric_cov = svd.singularValues().tail<3>();
    if (frame_count >= WINDOW_SIZE && ric_cov(1) > 0.25) {
        calib_ric = ric;
        return true;
    } else {
        return false;
    }
}

// 根据匹配的特征点计算帧间相机旋转矩阵Rc
Matrix3d InitialEXRotation::solveRelativeR(
        const vector<pair<Vector3d, Vector3d>>& corres) {
    // 8点法以上的数据进行金酸
    if (corres.size() >= 9) {
        vector<cv::Point2f> ll, rr;
        for (int i = 0; i < int(corres.size()); ++i) {
            ll.push_back(cv::Point2f(corres[i].first(0), corres[i].first(1)));
            rr.push_back(cv::Point2f(corres[i].second(0), corres[i].second(1)));
        }
        cv::Mat E = cv::findFundamentalMat(ll, rr); // 计算基础矩阵
        cv::Mat_<double> R1, R2, t1, t2;
        decomposeE(E, R1, R2, t1, t2);

        if (determinant(R1) + 1.0 < 1e-09) { // 如果行列式小于0,则调整矩阵E重新计算
            E = -E;
            decomposeE(E, R1, R2, t1, t2);
        }
        // 通过三角化点查看深度大于0个数的方法,判断svd分解得到的正确旋转矩阵
        double ratio1 = max(testTriangulation(ll, rr, R1, t1), testTriangulation(ll, rr, R1, t2));
        double ratio2 = max(testTriangulation(ll, rr, R2, t1), testTriangulation(ll, rr, R2, t2));
        cv::Mat_<double> ans_R_cv = ratio1 > ratio2 ? R1 : R2;

        // 对cv计算的旋转矩阵求转置
        Matrix3d ans_R_eigen;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                ans_R_eigen(j, i) = ans_R_cv(i, j);
        return ans_R_eigen;
    }
    return Matrix3d::Identity();
}

// 判断不同旋转矩阵的三角化点的正深度个数比例
double InitialEXRotation::testTriangulation(const vector<cv::Point2f>& l,
                                            const vector<cv::Point2f>& r,
                                            cv::Mat_<double>& R, cv::Mat_<double>& t) {
    cv::Mat pointcloud;
    cv::Matx34f P = cv::Matx34f(1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, 1, 0);
    cv::Matx34f P1 = cv::Matx34f(R(0, 0), R(0, 1), R(0, 2), t(0),
                                 R(1, 0), R(1, 1), R(1, 2), t(1),
                                 R(2, 0), R(2, 1), R(2, 2), t(2));
    cv::triangulatePoints(P, P1, l, r, pointcloud);
    int front_count = 0;
    for (int i = 0; i < pointcloud.cols; i++) {
        double normal_factor = pointcloud.col(i).at<float>(3);

        cv::Mat_<double> p_3d_l = cv::Mat(P) * (pointcloud.col(i) / normal_factor);
        cv::Mat_<double> p_3d_r = cv::Mat(P1) * (pointcloud.col(i) / normal_factor);
        if (p_3d_l(2) > 0 && p_3d_r(2) > 0)
            front_count++;
    }
    return 1.0 * front_count / pointcloud.cols;
}

// SVD分解得出R和t
void InitialEXRotation::decomposeE(cv::Mat& E,
                                   cv::Mat_<double>& R1, cv::Mat_<double>& R2,
                                   cv::Mat_<double>& t1, cv::Mat_<double>& t2) {
    cv::SVD svd(E, cv::SVD::MODIFY_A); // 使用这个选项会改变E
    cv::Matx33d W(0, -1, 0,
                  1, 0, 0,
                  0, 0, 1);
    cv::Matx33d Wt(0, 1, 0,
                   -1, 0, 0,
                   0, 0, 1);
    R1 = svd.u * cv::Mat(W) * svd.vt;
    R2 = svd.u * cv::Mat(Wt) * svd.vt;
    t1 = svd.u.col(2);
    t2 = -svd.u.col(2);
}